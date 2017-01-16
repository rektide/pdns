/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2003 - 2016  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation

    Additionally, the license of this program contains a special
    exception which allows to distribute the program in binary form when
    it is linked against OpenSSL.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "ws-recursor.hh"
#include "json.hh"

#include <string>
#include "namespaces.hh"
#include <iostream>
#include "iputils.hh"
#include "rec_channel.hh"
#include "arguments.hh"
#include "misc.hh"
#include "syncres.hh"
#include "dnsparser.hh"
#include "json11.hpp"
#include "webserver.hh"
#include "ws-api.hh"
#include "logger.hh"
#include "ext/incbin/incbin.h"

extern __thread FDMultiplexer* t_fdm;

using json11::Json;

void productServerStatisticsFetch(map<string,uint64_t>& out)
{
  map<string,uint64_t> stats = getAllStatsMap();
  out.swap(stats);
}

static void apiWriteConfigFile(const string& filebasename, const string& content)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  string filename = ::arg()["api-config-dir"] + "/" + filebasename + ".conf";
  ofstream ofconf(filename.c_str());
  if (!ofconf) {
    throw ApiException("Could not open config fragment file '"+filename+"' for writing: "+stringerror());
  }
  ofconf << "# Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
  ofconf << content << endl;
  ofconf.close();
}

static void apiServerConfigAllowFrom(HttpRequest* req, HttpResponse* resp)
{
  if (req->method == "PUT" && !::arg().mustDo("api-readonly")) {
    Json document = req->json();

    auto jlist = document["value"];
    if (!jlist.is_array()) {
      throw ApiException("'value' must be an array");
    }

    for (auto value : jlist.array_items()) {
      try {
        Netmask(value.string_value());
      } catch (NetmaskException &e) {
        throw ApiException(e.reason);
      }
    }

    ostringstream ss;

    // Clear allow-from-file if set, so our changes take effect
    ss << "allow-from-file=" << endl;

    // Clear allow-from, and provide a "parent" value
    ss << "allow-from=" << endl;
    for (auto value : jlist.array_items()) {
      ss << "allow-from+=" << value.string_value() << endl;
    }

    apiWriteConfigFile("allow-from", ss.str());

    parseACLs();

    // fall through to GET
  } else if (req->method != "GET") {
    throw HttpMethodNotAllowedException();
  }

  // Return currently configured ACLs
  vector<string> entries;
  t_allowFrom->toStringVector(&entries);

  resp->setBody(Json::object {
    { "name", "allow-from" },
    { "value", entries },
  });
}

static void fillZone(const DNSName& zonename, HttpResponse* resp)
{
  auto iter = t_sstorage->domainmap->find(zonename);
  if (iter == t_sstorage->domainmap->end())
    throw ApiException("Could not find domain '"+zonename.toString()+"'");

  const SyncRes::AuthDomain& zone = iter->second;

  Json::array servers;
  for(const ComboAddress& server : zone.d_servers) {
    servers.push_back(server.toStringWithPort());
  }

  Json::array records;
  for(const SyncRes::AuthDomain::records_t::value_type& dr : zone.d_records) {
    records.push_back(Json::object {
      { "name", dr.d_name.toString() },
      { "type", DNSRecordContent::NumberToType(dr.d_type) },
      { "ttl", (double)dr.d_ttl },
      { "content", dr.d_content->getZoneRepresentation() }
    });
  }

  // id is the canonical lookup key, which doesn't actually match the name (in some cases)
  string zoneId = apiZoneNameToId(iter->first);
  Json::object doc = {
    { "id", zoneId },
    { "url", "/api/v1/servers/localhost/zones/" + zoneId },
    { "name", iter->first.toString() },
    { "kind", zone.d_servers.empty() ? "Native" : "Forwarded" },
    { "servers", servers },
    { "recursion_desired", zone.d_servers.empty() ? false : zone.d_rdForward },
    { "records", records }
  };

  resp->setBody(doc);
}

static void doCreateZone(const Json document)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  DNSName zonename = apiNameToDNSName(stringFromJson(document, "name"));
  apiCheckNameAllowedCharacters(zonename.toString());

  string singleIPTarget = document["single_target_ip"].string_value();
  string kind = toUpper(stringFromJson(document, "kind"));
  bool rd = boolFromJson(document, "recursion_desired");
  string confbasename = "zone-" + apiZoneNameToId(zonename);

  if (kind == "NATIVE") {
    if (rd)
      throw ApiException("kind=Native and recursion_desired are mutually exclusive");
    if(!singleIPTarget.empty()) {
      try {
	ComboAddress rem(singleIPTarget);
	if(rem.sin4.sin_family != AF_INET)
	  throw ApiException("");
	singleIPTarget = rem.toString();
      }
      catch(...) {
	throw ApiException("Single IP target '"+singleIPTarget+"' is invalid");
      }
    }
    string zonefilename = ::arg()["api-config-dir"] + "/" + confbasename + ".zone";
    ofstream ofzone(zonefilename.c_str());
    if (!ofzone) {
      throw ApiException("Could not open '"+zonefilename+"' for writing: "+stringerror());
    }
    ofzone << "; Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
    ofzone << zonename << "\tIN\tSOA\tlocal.zone.\thostmaster."<<zonename<<" 1 1 1 1 1" << endl;
    if(!singleIPTarget.empty()) {
      ofzone <<zonename << "\t3600\tIN\tA\t"<<singleIPTarget<<endl;
      ofzone <<"*."<<zonename << "\t3600\tIN\tA\t"<<singleIPTarget<<endl;
    }
    ofzone.close();

    apiWriteConfigFile(confbasename, "auth-zones+=" + zonename.toString() + "=" + zonefilename);
  } else if (kind == "FORWARDED") {
    string serverlist;
    for (auto value : document["servers"].array_items()) {
      string server = value.string_value();
      if (server == "") {
        throw ApiException("Forwarded-to server must not be an empty string");
      }
      if (!serverlist.empty()) {
        serverlist += ";";
      }
      serverlist += server;
    }
    if (serverlist == "")
      throw ApiException("Need at least one upstream server when forwarding");

    if (rd) {
      apiWriteConfigFile(confbasename, "forward-zones-recurse+=" + zonename.toString() + "=" + serverlist);
    } else {
      apiWriteConfigFile(confbasename, "forward-zones+=" + zonename.toString() + "=" + serverlist);
    }
  } else {
    throw ApiException("invalid kind");
  }
}

static bool doDeleteZone(const DNSName& zonename)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  string filename;

  // this one must exist
  filename = ::arg()["api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".conf";
  if (unlink(filename.c_str()) != 0) {
    return false;
  }

  // .zone file is optional
  filename = ::arg()["api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".zone";
  unlink(filename.c_str());

  return true;
}

static void apiServerZones(HttpRequest* req, HttpResponse* resp)
{
  if (req->method == "POST" && !::arg().mustDo("api-readonly")) {
    if (::arg()["api-config-dir"].empty()) {
      throw ApiException("Config Option \"api-config-dir\" must be set");
    }

    Json document = req->json();

    DNSName zonename = apiNameToDNSName(stringFromJson(document, "name"));

    auto iter = t_sstorage->domainmap->find(zonename);
    if (iter != t_sstorage->domainmap->end())
      throw ApiException("Zone already exists");

    doCreateZone(document);
    reloadAuthAndForwards();
    fillZone(zonename, resp);
    resp->status = 201;
    return;
  }

  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  Json::array doc;
  for(const SyncRes::domainmap_t::value_type& val :  *t_sstorage->domainmap) {
    const SyncRes::AuthDomain& zone = val.second;
    Json::array servers;
    for(const ComboAddress& server : zone.d_servers) {
      servers.push_back(server.toStringWithPort());
    }
    // id is the canonical lookup key, which doesn't actually match the name (in some cases)
    string zoneId = apiZoneNameToId(val.first);
    doc.push_back(Json::object {
      { "id", zoneId },
      { "url", "/api/v1/servers/localhost/zones/" + zoneId },
      { "name", val.first.toString() },
      { "kind", zone.d_servers.empty() ? "Native" : "Forwarded" },
      { "servers", servers },
      { "recursion_desired", zone.d_servers.empty() ? false : zone.d_rdForward }
    });
  }
  resp->setBody(doc);
}

static void apiServerZoneDetail(HttpRequest* req, HttpResponse* resp)
{
  DNSName zonename = apiZoneIdToName(req->parameters["id"]);

  SyncRes::domainmap_t::const_iterator iter = t_sstorage->domainmap->find(zonename);
  if (iter == t_sstorage->domainmap->end())
    throw ApiException("Could not find domain '"+zonename.toString()+"'");

  if(req->method == "PUT" && !::arg().mustDo("api-readonly")) {
    Json document = req->json();

    doDeleteZone(zonename);
    doCreateZone(document);
    reloadAuthAndForwards();
    resp->body = "";
    resp->status = 204; // No Content, but indicate success
  }
  else if(req->method == "DELETE" && !::arg().mustDo("api-readonly")) {
    if (!doDeleteZone(zonename)) {
      throw ApiException("Deleting domain failed");
    }

    reloadAuthAndForwards();
    // empty body on success
    resp->body = "";
    resp->status = 204; // No Content: declare that the zone is gone now
  } else if(req->method == "GET") {
    fillZone(zonename, resp);
  } else {
    throw HttpMethodNotAllowedException();
  }
}

static void apiServerSearchData(HttpRequest* req, HttpResponse* resp) {
  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  string q = req->getvars["q"];
  if (q.empty())
    throw ApiException("Query q can't be blank");

  Json::array doc;
  for(const SyncRes::domainmap_t::value_type& val : *t_sstorage->domainmap) {
    string zoneId = apiZoneNameToId(val.first);
    string zoneName = val.first.toString();
    if (pdns_ci_find(zoneName, q) != string::npos) {
      doc.push_back(Json::object {
        { "type", "zone" },
        { "zone_id", zoneId },
        { "name", zoneName }
      });
    }

    // if zone name is an exact match, don't bother with returning all records/comments in it
    if (val.first == DNSName(q)) {
      continue;
    }

    const SyncRes::AuthDomain& zone = val.second;

    for(const SyncRes::AuthDomain::records_t::value_type& rr : zone.d_records) {
      if (pdns_ci_find(rr.d_name.toString(), q) == string::npos && pdns_ci_find(rr.d_content->getZoneRepresentation(), q) == string::npos)
        continue;

      doc.push_back(Json::object {
        { "type", "record" },
        { "zone_id", zoneId },
        { "zone_name", zoneName },
        { "name", rr.d_name.toString() },
        { "content", rr.d_content->getZoneRepresentation() }
      });
    }
  }
  resp->setBody(doc);
}

static void apiServerCacheFlush(HttpRequest* req, HttpResponse* resp) {
  if(req->method != "PUT")
    throw HttpMethodNotAllowedException();

  DNSName canon = apiNameToDNSName(req->getvars["domain"]);

  int count = broadcastAccFunction<uint64_t>(boost::bind(pleaseWipeCache, canon, false));
  count += broadcastAccFunction<uint64_t>(boost::bind(pleaseWipePacketCache, canon, false));
  count += broadcastAccFunction<uint64_t>(boost::bind(pleaseWipeAndCountNegCache, canon, false));
  resp->setBody(Json::object {
    { "count", count },
    { "result", "Flushed cache." }
  });
}

#include "htmlfiles.h"

void serveStuff(HttpRequest* req, HttpResponse* resp) 
{
  resp->headers["Cache-Control"] = "max-age=86400";

  if(req->url.path == "/")
    req->url.path = "/index.html";

  const string charset = "; charset=utf-8";
  if(boost::ends_with(req->url.path, ".html"))
    resp->headers["Content-Type"] = "text/html" + charset;
  else if(boost::ends_with(req->url.path, ".css"))
    resp->headers["Content-Type"] = "text/css" + charset;
  else if(boost::ends_with(req->url.path,".js"))
    resp->headers["Content-Type"] = "application/javascript" + charset;
  else if(boost::ends_with(req->url.path, ".png"))
    resp->headers["Content-Type"] = "image/png";

  resp->headers["X-Content-Type-Options"] = "nosniff";
  resp->headers["X-Frame-Options"] = "deny";
  resp->headers["X-Permitted-Cross-Domain-Policies"] = "none";

  resp->headers["X-XSS-Protection"] = "1; mode=block";
  //  resp->headers["Content-Security-Policy"] = "default-src 'self'; style-src 'self' 'unsafe-inline'";

  resp->body = g_urlmap[req->url.path.c_str()+1];
  resp->status = 200;
}


RecursorWebServer::RecursorWebServer(FDMultiplexer* fdm)
{
  RecursorControlParser rcp; // inits

  d_ws = new AsyncWebServer(fdm, arg()["webserver-address"], arg().asNum("webserver-port"));
  d_ws->bind();

  // legacy dispatch
  d_ws->registerApiHandler("/jsonstat", boost::bind(&RecursorWebServer::jsonstat, this, _1, _2));
  d_ws->registerApiHandler("/api/v1/servers/localhost/cache/flush", &apiServerCacheFlush);
  d_ws->registerApiHandler("/api/v1/servers/localhost/config/allow-from", &apiServerConfigAllowFrom);
  d_ws->registerApiHandler("/api/v1/servers/localhost/config", &apiServerConfig);
  d_ws->registerApiHandler("/api/v1/servers/localhost/search-log", &apiServerSearchLog);
  d_ws->registerApiHandler("/api/v1/servers/localhost/search-data", &apiServerSearchData);
  d_ws->registerApiHandler("/api/v1/servers/localhost/statistics", &apiServerStatistics);
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones/<id>", &apiServerZoneDetail);
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones", &apiServerZones);
  d_ws->registerApiHandler("/api/v1/servers/localhost", &apiServerDetail);
  d_ws->registerApiHandler("/api/v1/servers", &apiServer);
  d_ws->registerApiHandler("/api", &apiDiscovery);

  for(const auto& u : g_urlmap) 
    d_ws->registerWebHandler("/"+u.first, serveStuff);
  d_ws->registerWebHandler("/", serveStuff);
  d_ws->go();
}

void RecursorWebServer::jsonstat(HttpRequest* req, HttpResponse *resp)
{
  string command;

  if(req->getvars.count("command")) {
    command = req->getvars["command"];
    req->getvars.erase("command");
  }

  map<string, string> stats;
  if(command == "get-query-ring") {
    typedef pair<DNSName,uint16_t> query_t;
    vector<query_t> queries;
    bool filter=!req->getvars["public-filtered"].empty();

    if(req->getvars["name"]=="servfail-queries")
      queries=broadcastAccFunction<vector<query_t> >(pleaseGetServfailQueryRing);
    else if(req->getvars["name"]=="queries")
      queries=broadcastAccFunction<vector<query_t> >(pleaseGetQueryRing);

    typedef map<query_t,unsigned int> counts_t;
    counts_t counts;
    unsigned int total=0;
    for(const query_t& q :  queries) {
      total++;
      if(filter)
	counts[make_pair(getRegisteredName(q.first), q.second)]++;
      else
	counts[make_pair(q.first, q.second)]++;
    }

    typedef std::multimap<int, query_t> rcounts_t;
    rcounts_t rcounts;

    for(counts_t::const_iterator i=counts.begin(); i != counts.end(); ++i)
      rcounts.insert(make_pair(-i->second, i->first));

    Json::array entries;
    unsigned int tot=0, totIncluded=0;
    for(const rcounts_t::value_type& q :  rcounts) {
      totIncluded-=q.first;
      entries.push_back(Json::array {
        -q.first, q.second.first.toString(), DNSRecordContent::NumberToType(q.second.second)
      });
      if(tot++>=100)
	break;
    }
    if(queries.size() != totIncluded) {
      entries.push_back(Json::array {
        (int)(queries.size() - totIncluded), "", ""
      });
    }
    resp->setBody(Json::object { { "entries", entries } });
    return;
  }
  else if(command == "get-remote-ring") {
    vector<ComboAddress> queries;
    if(req->getvars["name"]=="remotes")
      queries=broadcastAccFunction<vector<ComboAddress> >(pleaseGetRemotes);
    else if(req->getvars["name"]=="servfail-remotes")
      queries=broadcastAccFunction<vector<ComboAddress> >(pleaseGetServfailRemotes);
    else if(req->getvars["name"]=="large-answer-remotes")
      queries=broadcastAccFunction<vector<ComboAddress> >(pleaseGetLargeAnswerRemotes);

    typedef map<ComboAddress,unsigned int,ComboAddress::addressOnlyLessThan> counts_t;
    counts_t counts;
    unsigned int total=0;
    for(const ComboAddress& q :  queries) {
      total++;
      counts[q]++;
    }

    typedef std::multimap<int, ComboAddress> rcounts_t;
    rcounts_t rcounts;

    for(counts_t::const_iterator i=counts.begin(); i != counts.end(); ++i)
      rcounts.insert(make_pair(-i->second, i->first));

    Json::array entries;
    unsigned int tot=0, totIncluded=0;
    for(const rcounts_t::value_type& q :  rcounts) {
      totIncluded-=q.first;
      entries.push_back(Json::array {
        -q.first, q.second.toString()
      });
      if(tot++>=100)
	break;
    }
    if(queries.size() != totIncluded) {
      entries.push_back(Json::array {
        (int)(queries.size() - totIncluded), "", ""
      });
    }

    resp->setBody(Json::object { { "entries", entries } });
    return;
  } else {
    resp->setErrorResult("Command '"+command+"' not found", 404);
  }
}


void AsyncServerNewConnectionMT(void *p) {
  AsyncServer *server = (AsyncServer*)p;
  try {
    Socket* socket = server->accept();
    server->d_asyncNewConnectionCallback(socket);
    delete socket;
  } catch (NetworkError &e) {
    // we're running in a shared process/thread, so can't just terminate/abort.
    return;
  }
}

void AsyncServer::asyncWaitForConnections(FDMultiplexer* fdm, const newconnectioncb_t& callback)
{
  d_asyncNewConnectionCallback = callback;
  fdm->addReadFD(d_server_socket.getHandle(), boost::bind(&AsyncServer::newConnection, this));
}

void AsyncServer::newConnection()
{
  MT->makeThread(&AsyncServerNewConnectionMT, this);
}


void AsyncWebServer::serveConnection(Socket *client)
{
  HttpRequest req;
  YaHTTP::AsyncRequestLoader yarl;
  yarl.initialize(&req);
  client->setNonBlocking();

  string data;
  try {
    while(!req.complete) {
      data.clear();
      int bytes = arecvtcp(data, 16384, client, true);
      if (bytes > 0) {
        req.complete = yarl.feed(data);
      } else {
        // read error OR EOF
        break;
      }
    }
    yarl.finalize();
  } catch (YaHTTP::ParseError &e) {
    // request stays incomplete
  }

  HttpResponse resp;
  handleRequest(req, resp);
  ostringstream ss;
  resp.write(ss);
  data = ss.str();

  // now send the reply
  if (asendtcp(data, client) == -1 || data.empty()) {
    L<<Logger::Error<<"Failed sending reply to HTTP client"<<endl;
  }
}

void AsyncWebServer::go() {
  if (!d_server)
    return;
  ((AsyncServer*)d_server)->asyncWaitForConnections(d_fdm, boost::bind(&AsyncWebServer::serveConnection, this, _1));
}
