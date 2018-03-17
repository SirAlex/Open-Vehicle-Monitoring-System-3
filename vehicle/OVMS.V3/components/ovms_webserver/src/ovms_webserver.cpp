/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;    (C) 2018       Michael Balzer
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "ovms_log.h"
static const char *TAG = "webserver";

#include <string.h>
#include <stdio.h>
#include "ovms_webserver.h"
#include "ovms_config.h"
#include "ovms_metrics.h"
#include "metrics_standard.h"
#include "buffered_shell.h"
#include "vehicle.h"


OvmsWebServer MyWebServer __attribute__ ((init_priority (8200)));

OvmsWebServer::OvmsWebServer()
{
  ESP_LOGI(TAG, "Initialising WEBSERVER (8200)");

  m_running = false;
  memset(m_sessions, 0, sizeof(m_sessions));

#if MG_ENABLE_FILESYSTEM
  m_file_enable = true;
  memset(&m_file_opts, 0, sizeof(m_file_opts));
#endif //MG_ENABLE_FILESYSTEM

  m_client_cnt = 0;
  m_client_mutex = xSemaphoreCreateMutex();
  m_update_ticker = xTimerCreate("Web client update ticker", 250 / portTICK_PERIOD_MS, pdTRUE, NULL, UpdateTicker);
  
  // read config:
  MyConfig.RegisterParam("http.server", "Webserver", true, true);
  ConfigChanged("init", NULL);

  #undef bind  // Kludgy, but works
  using std::placeholders::_1;
  using std::placeholders::_2;
  MyEvents.RegisterEvent(TAG,"network.mgr.init", std::bind(&OvmsWebServer::NetManInit, this, _1, _2));
  MyEvents.RegisterEvent(TAG,"network.mgr.stop", std::bind(&OvmsWebServer::NetManStop, this, _1, _2));
  MyEvents.RegisterEvent(TAG, "config.changed", std::bind(&OvmsWebServer::ConfigChanged, this, _1, _2));
  MyEvents.RegisterEvent(TAG, "config.mounted", std::bind(&OvmsWebServer::ConfigChanged, this, _1, _2));
  MyEvents.RegisterEvent(TAG, "*", std::bind(&OvmsWebServer::EventListener, this, _1, _2));
  
  // register standard framework URIs:
  RegisterPage("/", "OVMS", HandleRoot);
  RegisterPage("/assets/style.css", "style.css", HandleAsset);
  RegisterPage("/assets/script.js", "script.js", HandleAsset);
  RegisterPage("/assets/bootstrap.min.css.map", "-", HandleAsset);
  RegisterPage("/favicon.ico", "favicon.ico", HandleAsset);
  RegisterPage("/apple-touch-icon.png", "apple-touch-icon.png", HandleAsset);
  RegisterPage("/menu", "Menu", HandleMenu);
  RegisterPage("/home", "Home", HandleHome);
  RegisterPage("/login", "Login", HandleLogin);
  RegisterPage("/logout", "Logout", HandleLogout);
  
  // register standard API calls:
  RegisterPage("/api/execute", "Execute command", HandleCommand, PageMenu_None, PageAuth_Cookie);
  
  // register standard administration pages:
  RegisterPage("/status", "Status", HandleStatus, PageMenu_Main, PageAuth_Cookie);
  RegisterPage("/shell", "Shell", HandleShell, PageMenu_Main, PageAuth_Cookie);
  RegisterPage("/cfg/password", "Password", HandleCfgPassword, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/vehicle", "Vehicle", HandleCfgVehicle, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/wifi", "Wifi", HandleCfgWifi, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/modem", "Modem", HandleCfgModem, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/server/v2", "Server V2 (MP)", HandleCfgServerV2, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/server/v3", "Server V3 (MQTT)", HandleCfgServerV3, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/webserver", "Webserver", HandleCfgWebServer, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/autostart", "Autostart", HandleCfgAutoInit, PageMenu_Config, PageAuth_Cookie);
  RegisterPage("/cfg/firmware", "Firmware", HandleCfgFirmware, PageMenu_Config, PageAuth_Cookie);
}

OvmsWebServer::~OvmsWebServer()
{
  // never destroyed
}


void OvmsWebServer::NetManInit(std::string event, void* data)
{
  // Only initialise server for WIFI connections
  if (!MyNetManager.m_connected_wifi) return;

  m_running = true;
  ESP_LOGI(TAG,"Launching Web Server");

  struct mg_mgr* mgr = MyNetManager.GetMongooseMgr();

  char *error_string;
  struct mg_bind_opts bind_opts = {};
  bind_opts.error_string = (const char**) &error_string;
  struct mg_connection *nc = mg_bind_opt(mgr, ":80", EventHandler, bind_opts);
  if (!nc)
    ESP_LOGE(TAG, "Cannot bind to port 80: %s", error_string);
  else
    mg_set_protocol_http_websocket(nc);
}

void OvmsWebServer::NetManStop(std::string event, void* data)
{
  if (m_running) {
    ESP_LOGI(TAG,"Stopping Web Server");
    m_running = false;
  }
}


/**
 * ConfigChanged: read & apply configuration updates
 */
void OvmsWebServer::ConfigChanged(std::string event, void* data)
{
#if MG_ENABLE_FILESYSTEM
  OvmsConfigParam* param = (OvmsConfigParam*) data;
  ESP_LOGD(TAG, "ConfigChanged: %s %s", event.c_str(), param ? param->GetName().c_str() : "");
  
  if (!param || param->GetName() == "password") {
    UpdateGlobalAuthFile();
  }
  
  if (!param || param->GetName() == "http.server") {
    // Instances:
    //    Name                Default                 Function
    //    enable.files        yes                     Enable file serving from docroot
    //    enable.dirlist      yes                     Enable directory listings
    //    docroot             /sd                     File server document root
    //    auth.domain         ovms                    Default auth domain (digest realm)
    //    auth.file           .htpasswd               Per directory auth file (Note: no inheritance from parent dir!)
    //    auth.global         yes                     Use global auth for files (user "admin", module password)
    
    if (m_file_opts.document_root)
      free((void*)m_file_opts.document_root);
    if (m_file_opts.auth_domain)
      free((void*)m_file_opts.auth_domain);
    if (m_file_opts.per_directory_auth_file)
      free((void*)m_file_opts.per_directory_auth_file);
    
    m_file_enable =
      MyConfig.GetParamValueBool("http.server", "enable.files", true);
    m_file_opts.enable_directory_listing =
      MyConfig.GetParamValueBool("http.server", "enable.dirlist", true) ? "yes" : "no";
    m_file_opts.document_root =
      strdup(MyConfig.GetParamValue("http.server", "docroot", "/sd").c_str());
    
    m_file_opts.auth_domain =
      strdup(MyConfig.GetParamValue("http.server", "auth.domain", "ovms").c_str());
    m_file_opts.per_directory_auth_file =
      strdup(MyConfig.GetParamValue("http.server", "auth.file", ".htpasswd").c_str());
    m_file_opts.global_auth_file =
      MyConfig.GetParamValueBool("http.server", "auth.global", true) ? OVMS_GLOBAL_AUTH_FILE : NULL;
  }
#endif //MG_ENABLE_FILESYSTEM
}


/**
 * UpdateGlobalAuthFile: create digest auth for main user "admin"
 *    if password is set and global auth is activated
 */
void OvmsWebServer::UpdateGlobalAuthFile()
{
#if MG_ENABLE_FILESYSTEM
  if (!m_file_opts.global_auth_file || !m_file_opts.auth_domain)
    return;
  
  std::string p = MyConfig.GetParamValue("password", "module");
  
  if (p.empty())
  {
    unlink(m_file_opts.global_auth_file);
    ESP_LOGW(TAG, "UpdateGlobalAuthFile: no password set => no auth for web console");
  }
  else
  {
    FILE *fp = fopen(m_file_opts.global_auth_file, "w");
    if (!fp) {
      ESP_LOGE(TAG, "UpdateGlobalAuthFile: can't write to '%s'", m_file_opts.global_auth_file);
      return;
    }
    std::string auth = MakeDigestAuth(m_file_opts.auth_domain, "admin", p.c_str());
    fprintf(fp, "%s\n", auth.c_str());
    ESP_LOGD(TAG, "UpdateGlobalAuthFile: %s", auth.c_str());
    fclose(fp);
  }
#endif //MG_ENABLE_FILESYSTEM
}

const std::string OvmsWebServer::MakeDigestAuth(const char* realm, const char* username, const char* password)
{
  std::string line;
  line = username;
  line += ":";
  line += realm;
  line += ":";
  line += password;
  
  unsigned char digest[16];
  cs_md5_ctx md5_ctx;
  cs_md5_init(&md5_ctx);
  cs_md5_update(&md5_ctx, (const unsigned char*) line.data(), line.length());
  cs_md5_final(digest, &md5_ctx);
  
  char hex[33];
  cs_to_hex(hex, digest, sizeof(digest));
  
  line = username;
  line += ":";
  line += realm;
  line += ":";
  line += hex;
  return line;
}


/**
 * ExecuteCommand: BufferedShell wrapper
 */
const std::string OvmsWebServer::ExecuteCommand(const std::string command, int verbosity /*=COMMAND_RESULT_NORMAL*/)
{
  std::string output;
  BufferedShell* bs = new BufferedShell(false, verbosity);
  if (!bs)
    return output;
  bs->SetSecure(true); // Note: assuming user is admin
  output = "";
  bs->ProcessChars(command.data(), command.size());
  bs->ProcessChar('\n');
  bs->Dump(output);
  delete bs;
  return output;
}


/**
 * RegisterPage: add a page to the URI handler map
 * Note: use PageMenu_Vehicle for vehicle specific pages.
 */
void OvmsWebServer::RegisterPage(const char* uri, const char* label, PageHandler_t handler,
  PageMenu_t menu /*=PageMenu_None*/, PageAuth_t auth /*=PageAuth_None*/)
{
  auto prev = m_pagemap.before_begin();
  for (auto it = m_pagemap.begin(); it != m_pagemap.end(); prev = it++) {
    if (strcmp((*it)->uri, uri) == 0) {
      ESP_LOGE(TAG, "RegisterPage: second registration for uri '%s' (ignored)", uri);
      return;
    }
  }
  m_pagemap.insert_after(prev, new PageEntry(uri, label, handler, menu, auth));
}

void OvmsWebServer::DeregisterPage(const char* uri)
{
  auto prev = m_pagemap.before_begin();
  for (auto it = m_pagemap.begin(); it != m_pagemap.end(); prev = it++) {
    if (strcmp((*it)->uri, uri) == 0) {
      m_pagemap.erase_after(prev);
      break;
    }
  }
}

PageEntry* OvmsWebServer::FindPage(const char* uri)
{
  for (PageEntry* e : m_pagemap) {
    if (strcmp(e->uri, uri) == 0)
      return e;
  }
  return NULL;
}


/**
 * EventHandler: this is the mongoose main event handler.
 */
void OvmsWebServer::EventHandler(mg_connection *nc, int ev, void *p)
{
  PageContext_t c;
  MgHandler* handler = (MgHandler*) nc->user_data;
  
  //if (ev != MG_EV_POLL && ev != MG_EV_SEND)
  //if (nc->user_data)
  //  ESP_LOGV(TAG, "EventHandler: conn=%p handler=%p ev=%d p=%p rxbufsz=%d, txbufsz=%d", nc, nc->user_data, ev, p, nc->recv_mbuf.size, nc->send_mbuf.size);
  
  // call attached handler:
  if (handler)
    ev = handler->HandleEvent(ev, p);
  
  // framework handling:
  switch (ev)
  {
    case MG_EV_WEBSOCKET_HANDSHAKE_DONE:    // new websocket connection
      {
        MyWebServer.CreateWebSocketHandler(nc);
      }
      break;
    
    case MG_EV_HTTP_REQUEST:                // standard HTTP request
      {
        c.nc = nc;
        c.hm = (struct http_message *) p;
        c.session = MyWebServer.GetSession(c.hm);
        c.method.assign(c.hm->method.p, c.hm->method.len);
        c.uri.assign(c.hm->uri.p, c.hm->uri.len);
        ESP_LOGI(TAG, "HTTP %s %s", c.method.c_str(), c.uri.c_str());
        
        PageEntry* page = MyWebServer.FindPage(c.uri.c_str());
        if (page) {
          // serve by page handler:
          page->Serve(c);
        }
#if MG_ENABLE_FILESYSTEM
        else if (MyWebServer.m_file_enable) {
          // serve from file space:
          if (MyConfig.ProtectedPath(c.uri)) {
            mg_http_send_error(c.nc, 401, "Unauthorized");
            nc->flags |= MG_F_SEND_AND_CLOSE;
          }
          else {
            mg_serve_http(nc, c.hm, MyWebServer.m_file_opts);
          }
        }
#endif //MG_ENABLE_FILESYSTEM
        else {
          mg_http_send_error(c.nc, 404, "Not found");
          nc->flags |= MG_F_SEND_AND_CLOSE;
        }
      }
      break;
    
    case MG_EV_CLOSE:                       // connection has been closed
      {
        if (handler) {
          if (nc->flags & MG_F_IS_WEBSOCKET)
            MyWebServer.DestroyWebSocketHandler((WebSocketHandler*)handler);
          else
            delete handler;
        }
      }
      break;
    
    case MG_EV_TIMER:
      {
        // Perform session maintenance:
        MyWebServer.CheckSessions();
        mg_set_timer(nc, mg_time() + SESSION_CHECK_INTERVAL);
      }
      break;
    
    default:
      break;
  }
}


/**
 * PageEntry.Serve: check auth, call page handler
 */
void PageEntry::Serve(PageContext_t& c)
{
  // check auth:
  if
#if MG_ENABLE_FILESYSTEM
  (auth == PageAuth_Cookie && !MyConfig.GetParamValue("password", "module").empty())
#else
  (auth != PageAuth_None && !MyConfig.GetParamValue("password", "module").empty())
#endif
  {
    // use session cookie based auth:
    if (c.session == NULL)
    {
      MyWebServer.HandleLogin(*this, c);
      return;
    }
  }
#if MG_ENABLE_FILESYSTEM
  else if (auth == PageAuth_File)
  {
    // use mongoose htaccess file based digest auth:
    mg_serve_http_opts& opts = MyWebServer.m_file_opts;
    if (!mg_http_is_authorized(c.hm, c.hm->uri,
        opts.auth_domain, opts.global_auth_file,
        MG_AUTH_FLAG_IS_GLOBAL_PASS_FILE|MG_AUTH_FLAG_ALLOW_MISSING_FILE))
    {
      mg_http_send_digest_auth_request(c.nc, opts.auth_domain);
      return;
    }
  }
#endif //MG_ENABLE_FILESYSTEM
  
  // call page handler:
  size_t checkpoint1 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  handler(*this, c);
  size_t checkpoint2 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  ESP_LOGD(TAG, "Serve %s: %d bytes used, %d free", uri, checkpoint1-checkpoint2, checkpoint2);
}



/**
 * MgHandler.RequestPoll: init transmission from other context.
 * 
 * mg_broadcast() signals the mg_mgr_poll() task to send an MG_EV_POLL to all connections.
 */
void MgHandler::RequestPoll()
{
#if MG_ENABLE_BROADCAST
  if (!m_nc)
    return;
  
  if (xTaskGetCurrentTaskHandle() == MyNetManager.GetMongooseTaskHandle()) {
    // we're in the NetManTask, can send directly:
    HandleEvent(MG_EV_POLL, NULL);
  } else {
    MgHandler* origin = this;
    mg_broadcast(MyNetManager.GetMongooseMgr(), HandlePoll, &origin, sizeof(origin));
  }
#endif // MG_ENABLE_BROADCAST
}

void MgHandler::HandlePoll(mg_connection* nc, int ev, void* p)
{
  MgHandler* origin = *((MgHandler**)p);
  if (nc->user_data == origin)
    origin->HandleEvent(MG_EV_POLL, NULL);
}


/**
 * HttpDataSender: chunked transfer of a memory region (needs to be const during xfer)
 */
HttpDataSender::HttpDataSender(mg_connection* nc, const uint8_t* data, size_t size, bool keepalive /*=true*/)
: MgHandler(nc)
{
  m_data = data;
  m_size = size;
  m_sent = 0;
  m_keepalive = keepalive;
  ESP_LOGV(TAG, "HttpDataSender %p init (%d bytes)", m_data, m_size);
}

HttpDataSender::~HttpDataSender()
{
  if (m_sent < m_size)
    ESP_LOGV(TAG, "HttpDataSender %p abort, %d bytes sent", m_data, m_sent);
}

int HttpDataSender::HandleEvent(int ev, void* p)
{
  switch (ev)
  {
    case MG_EV_SEND:          // last transmission has finished
    {
      if (m_sent < m_size) {
        // send next chunk:
        size_t len = MIN(m_size - m_sent, XFER_CHUNK_SIZE);
        mg_send_http_chunk(m_nc, (const char*) m_data + m_sent, len);
        m_sent += len;
        //ESP_LOGV(TAG, "HttpDataSender %p sent %d/%d", m_data, m_sent, m_size);
      }
      else {
        // done:
        if (!m_keepalive)
          m_nc->flags |= MG_F_SEND_AND_CLOSE;
        mg_send_http_chunk(m_nc, "", 0);
        ESP_LOGV(TAG, "HttpDataSender %p done, %d bytes sent", m_data, m_sent);
        delete this;
      }
    }
    break;
    
    default:
      break;
  }
  
  return ev;
}


/**
 * HttpStringSender: chunked transfer of a memory region (needs to be const during xfer)
 */
HttpStringSender::HttpStringSender(mg_connection* nc, std::string* msg, bool keepalive /*=true*/)
  : MgHandler(nc)
{
  m_msg = msg;
  m_sent = 0;
  m_keepalive = keepalive;
  ESP_LOGV(TAG, "HttpStringSender %p init (%d bytes)", this, m_msg->size());
}

HttpStringSender::~HttpStringSender()
{
  if (m_sent < m_msg->size())
    ESP_LOGV(TAG, "HttpStringSender %p abort, %d bytes sent", this, m_sent);
  delete m_msg;
}

int HttpStringSender::HandleEvent(int ev, void* p)
{
  switch (ev)
  {
    case MG_EV_SEND:          // last transmission has finished
    {
      if (m_sent < m_msg->size()) {
        // send next chunk:
        size_t len = MIN(m_msg->size() - m_sent, XFER_CHUNK_SIZE);
        mg_send_http_chunk(m_nc, (const char*) m_msg->data() + m_sent, len);
        m_sent += len;
        //ESP_LOGV(TAG, "HttpStringSender %p sent %d/%d", this, m_sent, m_msg->size());
      }
      else {
        // done:
        if (!m_keepalive)
          m_nc->flags |= MG_F_SEND_AND_CLOSE;
        mg_send_http_chunk(m_nc, "", 0);
        ESP_LOGV(TAG, "HttpStringSender %p done, %d bytes sent", this, m_sent);
        delete this;
      }
    }
    break;
    
    default:
      break;
  }
  
  return ev;
}


/**
 * CheckLogin: check username & password
 * 
 * We use "admin" as a fixed username for now to be able to extend users later on
 * and be compatible with the file based digest authentication.
 */
bool OvmsWebServer::CheckLogin(std::string username, std::string password)
{
  std::string adminpass = MyConfig.GetParamValue("password", "module");
  return (adminpass.empty() || (username == "admin" && password == adminpass));
}


/**
 * GetSession: parses the session cookie and returns a pointer to the session struct
 * or NULL if not found.
 */
user_session* OvmsWebServer::GetSession(http_message *hm)
{
  user_session* session = NULL;
  struct mg_str *cookie_header = mg_get_http_header(hm, "cookie");
  if (cookie_header == NULL) return NULL;
  char ssid_buf[21], *ssid = ssid_buf;
  mg_http_parse_header2(cookie_header, SESSION_COOKIE_NAME, &ssid, sizeof(ssid_buf));
  uint64_t sid = strtoull(ssid, NULL, 16);
  if (sid != 0) {
    for (int i = 0; i < NUM_SESSIONS; i++) {
      if (m_sessions[i].id == sid) {
        m_sessions[i].last_used = mg_time();
        session = &m_sessions[i];
        break;
      }
    }
  }
  if (ssid != ssid_buf)
    free(ssid);
  return session;
}


/**
 * CreateSession: create a new session
 */
user_session* OvmsWebServer::CreateSession(const http_message *hm)
{
  /* Find first available slot or use the oldest one. */
  user_session *s = NULL;
  user_session *oldest_s = m_sessions;
  for (int i = 0; i < NUM_SESSIONS; i++) {
    if (m_sessions[i].id == 0) {
      s = &m_sessions[i];
      break;
    }
    if (m_sessions[i].last_used < oldest_s->last_used) {
      oldest_s = &m_sessions[i];
    }
  }
  if (s == NULL) {
    DestroySession(oldest_s);
    ESP_LOGD(TAG, "CreateSession: evicted %" INT64_X_FMT, oldest_s->id);
    s = oldest_s;
  }
  
  /* Initialize new session. */
  s->last_used = mg_time();
  
  /* Create an ID by putting various volatiles into a pot and stirring. */
  cs_sha1_ctx ctx;
  cs_sha1_init(&ctx);
  cs_sha1_update(&ctx, (const unsigned char *) hm->message.p, hm->message.len);
  cs_sha1_update(&ctx, (const unsigned char *) m_sessions, sizeof(m_sessions));
  std::string sys;
  sys = StdMetrics.ms_m_serial->AsString();
  cs_sha1_update(&ctx, (const unsigned char *) sys.data(), sys.size());
  sys = StdMetrics.ms_m_monotonic->AsString();
  cs_sha1_update(&ctx, (const unsigned char *) sys.data(), sys.size());
  sys = StdMetrics.ms_m_freeram->AsString();
  cs_sha1_update(&ctx, (const unsigned char *) sys.data(), sys.size());
  sys = StdMetrics.ms_v_bat_12v_voltage->AsString();
  cs_sha1_update(&ctx, (const unsigned char *) sys.data(), sys.size());
  
  unsigned char digest[20];
  cs_sha1_final(digest, &ctx);
  s->id = *((uint64_t *) digest);
  return s;
}


/**
 * DestroySession: delete (invalidate) a session
 */
void OvmsWebServer::DestroySession(user_session *s)
{
  memset(s, 0, sizeof(*s));
}


/**
 * CheckSessions: clean up sessions that have been idle for too long
 */
void OvmsWebServer::CheckSessions(void)
{
  time_t threshold = mg_time() - SESSION_TTL;
  for (int i = 0; i < NUM_SESSIONS; i++) {
    user_session *s = &m_sessions[i];
    if (s->id != 0 && s->last_used < threshold) {
      ESP_LOGD(TAG, "CheckSessions: session %" INT64_X_FMT " closed due to idleness.", s->id);
      DestroySession(s);
    }
  }
}


/**
 * HandleLogin: show/process login form
 * 
 * This handler serves all URIs protected by PageAuth_Cookie,
 *  redirects to original URI or /home as indicated.
 */
void OvmsWebServer::HandleLogin(PageEntry_t& p, PageContext_t& c)
{
  std::string error;
  
  if (c.method == "POST") {
    // validate login:
    std::string username = c.getvar("username");
    std::string password = c.getvar("password");
    user_session *s = NULL;
    
    if (!CheckLogin(username, password)) {
      error += "<li>Login validation failed, please check username &amp; password</li>";
      ESP_LOGW(TAG, "HandleLogin: auth failure for username '%s'", username.c_str());
    }
    else if ((s = MyWebServer.CreateSession(c.hm)) == NULL) {
      error += "<li>Session creation failed, please try again later</li>";
    }
    
    if (error == "") {
      // ok: set cookie, reload menu & redirect to original uri, /cfg/password or /home:
      if (MyConfig.GetParamValueBool("password", "changed") == false)
        c.uri = "/cfg/password";
      else if (c.uri == "/login" || c.uri == "/logout" || c.uri == "/")
        c.uri = "/home";
      
      char shead[200];
      snprintf(shead, sizeof(shead),
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Set-Cookie: %s=%" INT64_X_FMT "; path=/"
        , SESSION_COOKIE_NAME, s->id);
      c.head(200, shead);
      c.printf(
        "<script>$(\"#menu\").load(\"/menu\"); loaduri(\"#main\", \"get\", \"%s\", {})</script>"
        , c.uri.c_str());
      c.done();
      
      ESP_LOGI(TAG, "HandleLogin: '%s' logged in, sid %" INT64_X_FMT, username.c_str(), s->id);
      return;
    }
    
    // output error, return to form:
    error = "<p class=\"lead\">Error!</p><ul class=\"errorlist\">" + error + "</ul>";
    c.head(403);
    c.alert("danger", error.c_str());
  }
  else {
    if (c.uri != "/login") {
      c.head(403);
      c.alert("danger", "<p class=\"lead\">Login required</p>");
    } else {
      c.head(200);
    }
  }

  // generate form:
  c.panel_start("primary", "Login");
  c.form_start(c.uri.c_str());
  c.input_text("Username", "username", "", "Main user: 'admin'", NULL, "autocomplete=\"section-login username\"");
  c.input_password("Password", "password", "", NULL, NULL, "autocomplete=\"section-login current-password\"");
  c.input_button("default", "Login");
  c.form_end();
  c.panel_end();
  c.done();
}


/**
 * HandleLogout: remove cookie and associated session state
 */
void OvmsWebServer::HandleLogout(PageEntry_t& p, PageContext_t& c)
{
  user_session *s = MyWebServer.GetSession(c.hm);
  if (s != NULL) {
    ESP_LOGI(TAG, "HandleLogout: session %" INT64_X_FMT " destroyed", s->id);
    MyWebServer.DestroySession(s);
  }
  
  // erase cookie, reload menu & redirect to /home:
  char shead[200];
  snprintf(shead, sizeof(shead),
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-cache\r\n"
    "Set-Cookie: %s="
    , SESSION_COOKIE_NAME);
  c.head(200, shead);
  c.print(
    "<script>$(\"#menu\").load(\"/menu\"); loaduri(\"#main\", \"get\", \"/home\", {})</script>");
  c.done();
}
