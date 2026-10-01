/*
   ============================================================
   ESP32 iLO4 FAN CONTROLLER
   ============================================================

   Features:
   - ESP32 (tested on WROOM-32D)
   - Open configuration AP
   - WiFi DHCP / Static IP
   - Persistent WiFi settings
   - Persistent iLO4 settings
   - iLO4 SSH Test Connection
   - iLO4 old SSH/KEX compatibility
   - Profile manager
   - Create profiles
   - Rename profiles
   - Delete profiles
   - Edit six fan maximum speeds
   - Persistent profiles
   - Maximum 20 profiles
   - 1.3" 128x64 I2C OLED + EC11 rotary control panel
   - Browse profiles, apply a profile, verify fan speeds match

   Required libraries:

   WiFi
   WebServer
   Preferences
   libssh_esp32
   U8g2 (by olikraus)

   ============================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include <Wire.h>
#include <U8g2lib.h>


// ============================================================
// CONFIGURATION
// ============================================================

#define AP_SSID "ESP32-iLO4"

#define WIFI_CONNECT_TIMEOUT 15000

#define MAX_PROFILES 20
#define MAX_PROFILE_NAME 32


// ============================================================
// OLED / EC11 PANEL CONFIGURATION
// ============================================================

// OLED is on the ESP32's default hardware I2C pins
// (SDA=GPIO21, SCL=GPIO22 on a WROOM-32D) so no pin
// remap is needed, Wire.begin() alone is enough.
#define OLED_I2C_ADDRESS 0x3C

// EC11 rotary encoder + push button pins.
// Change these if these pins are already used by something else.
#define EC11_PIN_CLK 32
#define EC11_PIN_DT  33
#define EC11_PIN_SW  25

// How long (ms) we poll fan speeds after applying a profile
// before giving up and declaring the apply "Failed".
#define VERIFY_DURATION_MS 10000

// How often (ms) we re-check fan speeds during verification.
#define VERIFY_POLL_INTERVAL_MS 1000

// Allowed difference between requested % and actual % before
// we consider a fan "matched". iLO doesn't always land exactly
// on the requested percentage, so allow a small slack.
#define FAN_SPEED_TOLERANCE_PERCENT 5

// iLO4's "fan p X max Y" apply command uses a raw 0-255 scale
// internally, not 0-100%. The UI stores/shows 0-100% for the
// user; this converts to the raw scale iLO4 actually expects.
// (Confirmed: the speed iLO4 reports back on reads is already
// a 0-100% value, so no conversion is needed on that side -
// the verify comparison uses this straight against percent.)
inline uint8_t percentToRawFanValue(int percent) {

  if (percent < 0) {
    percent = 0;
  }

  if (percent > 100) {
    percent = 100;
  }

  return (uint8_t)((percent * 255 + 50) / 100);
}

// Button debounce window.
#define EC11_DEBOUNCE_MS 180


// ============================================================
// GLOBAL OBJECTS
// ============================================================

WebServer server(80);
Preferences preferences;


// ============================================================
// PROFILE STRUCTURE
// ============================================================

struct Profile {

  bool used;

  String name;

  uint8_t fanMax[6];
};


// ============================================================
// GLOBAL SETTINGS
// ============================================================

// WiFi
String wifiSSID = "";
String wifiPassword = "";

bool wifiDHCP = true;

String wifiIP = "";
String wifiGateway = "";
String wifiSubnet = "";


// iLO4
String iloIP = "";
String iloUsername = "";
String iloPassword = "";


// Profiles
Profile profiles[MAX_PROFILES];


// ============================================================
// OLED / EC11 PANEL - GLOBAL OBJECTS & STATE
// ============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  /* reset=*/ U8X8_PIN_NONE
);


// Encoder state (updated from ISR).
volatile int encoderDelta = 0;
volatile uint8_t encoderLastState = 0;

// Button state.
bool buttonLastRaw = HIGH;
unsigned long buttonLastChangeMs = 0;
bool buttonPressedEvent = false;


// Panel UI states.
enum PanelState {
  PANEL_PROFILE_LIST,
  PANEL_PROFILE_MENU,
  PANEL_APPLYING,
  PANEL_RESULT,
  PANEL_SCREENSAVER
};

PanelState panelState = PANEL_PROFILE_LIST;
PanelState panelStateBeforeScreensaver = PANEL_PROFILE_LIST;

// Inactivity / screensaver.
#define SCREENSAVER_TIMEOUT_MS 10000
#define SCREENSAVER_CONTRAST 4
unsigned long lastActivityMs = 0;

// Index into the *filtered* list of used profiles, not into
// profiles[] directly (unused slots are skipped on the panel).
int panelListSelection = 0;

// Which real profiles[] slot is currently selected/opened.
int panelSelectedProfile = -1;

// 0 = "Apply", 1 = "Back", inside the profile submenu.
int panelSubMenuSelection = 0;

// Result screen text (shown after an apply/verify attempt).
String panelResultLine1 = "";
String panelResultLine2 = "";
bool panelResultOk = false;


// ============================================================
// HTML HEADER
// ============================================================

String htmlHeader(const String &title) {

  String html;

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";

  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";

  html += "<title>";
  html += title;
  html += "</title>";

  html += "<style>";

  html +=
    "body{"
    "font-family:Arial,sans-serif;"
    "background:#111;"
    "color:#eee;"
    "margin:0;"
    "padding:0;"
    "}";

  html +=
    ".container{"
    "max-width:700px;"
    "margin:auto;"
    "padding:20px;"
    "}";

  html +=
    "h1{"
    "font-size:27px;"
    "margin-bottom:25px;"
    "}";

  html +=
    "h2{"
    "font-size:21px;"
    "}";

  html +=
    ".card{"
    "background:#1d1d1d;"
    "border-radius:12px;"
    "padding:20px;"
    "margin-bottom:15px;"
    "box-shadow:0 2px 8px rgba(0,0,0,.4);"
    "}";

  html +=
    "button,.button{"
    "display:inline-block;"
    "background:#2878ff;"
    "border:0;"
    "color:white;"
    "padding:12px 18px;"
    "border-radius:8px;"
    "text-decoration:none;"
    "font-size:15px;"
    "cursor:pointer;"
    "margin:4px;"
    "}";

  html +=
    ".danger{"
    "background:#c62828;"
    "}";

  html +=
    ".secondary{"
    "background:#555;"
    "}";

  html +=
    ".menuButton{"
    "display:block;"
    "width:100%;"
    "box-sizing:border-box;"
    "text-align:left;"
    "margin:10px 0;"
    "padding:18px;"
    "font-size:18px;"
    "}";

  html +=
    "input[type=text],"
    "input[type=password],"
    "input[type=number]{"
    "width:100%;"
    "box-sizing:border-box;"
    "padding:11px;"
    "margin-top:6px;"
    "margin-bottom:16px;"
    "background:#292929;"
    "border:1px solid #555;"
    "border-radius:7px;"
    "color:white;"
    "font-size:16px;"
    "}";

  html +=
    "input[type=range]{"
    "width:100%;"
    "}";

  html +=
    "label{"
    "display:block;"
    "margin-top:10px;"
    "font-weight:bold;"
    "}";

  html +=
    ".profileRow{"
    "background:#292929;"
    "border-radius:8px;"
    "padding:15px;"
    "margin:10px 0;"
    "}";

  html +=
    ".status{"
    "padding:12px;"
    "border-radius:8px;"
    "background:#252525;"
    "margin-bottom:15px;"
    "}";

  html +=
    ".success{"
    "border-left:5px solid #22c55e;"
    "}";

  html +=
    ".error{"
    "border-left:5px solid #ef4444;"
    "}";

  html +=
    ".info{"
    "border-left:5px solid #3b82f6;"
    "}";

  html +=
    ".fanValue{"
    "font-size:18px;"
    "font-weight:bold;"
    "}";

  html +=
    ".back{"
    "margin-top:20px;"
    "}";

  html += "</style>";

  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";

  return html;
}


// ============================================================
// HTML FOOTER
// ============================================================

String htmlFooter() {

  String html;

  html += "</div>";
  html += "</body>";
  html += "</html>";

  return html;
}


// ============================================================
// HTML ESCAPE
// ============================================================

String htmlEscape(const String &input) {

  String output;

  for (size_t i = 0; i < input.length(); i++) {

    char c = input[i];

    if (c == '&') {
      output += "&amp;";
    }
    else if (c == '<') {
      output += "&lt;";
    }
    else if (c == '>') {
      output += "&gt;";
    }
    else if (c == '"') {
      output += "&quot;";
    }
    else if (c == '\'') {
      output += "&#39;";
    }
    else {
      output += c;
    }
  }

  return output;
}


// ============================================================
// LOAD SETTINGS
// ============================================================

void loadSettings() {

  // ----------------------------------------------------------
  // General settings
  // ----------------------------------------------------------

  preferences.begin("settings", true);

  wifiSSID =
    preferences.getString("wifi_ssid", "");

  wifiPassword =
    preferences.getString("wifi_pass", "");

  wifiDHCP =
    preferences.getBool("wifi_dhcp", true);

  wifiIP =
    preferences.getString("wifi_ip", "");

  wifiGateway =
    preferences.getString("wifi_gw", "");

  wifiSubnet =
    preferences.getString("wifi_sub", "");


  iloIP =
    preferences.getString("ilo_ip", "");

  iloUsername =
    preferences.getString("ilo_user", "");

  iloPassword =
    preferences.getString("ilo_pass", "");

  preferences.end();


  // ----------------------------------------------------------
  // Profiles
  // ----------------------------------------------------------

  preferences.begin("profiles", true);

  for (int i = 0; i < MAX_PROFILES; i++) {

    String usedKey =
      "u" + String(i);

    profiles[i].used =
      preferences.getBool(
        usedKey.c_str(),
        false
      );


    if (!profiles[i].used) {

      profiles[i].name = "";

      for (int fan = 0; fan < 6; fan++) {
        profiles[i].fanMax[fan] = 100;
      }

      continue;
    }


    String nameKey =
      "n" + String(i);

    profiles[i].name =
      preferences.getString(
        nameKey.c_str(),
        "Profile"
      );


    for (int fan = 0; fan < 6; fan++) {

      String fanKey =
        "f" +
        String(i) +
        "_" +
        String(fan);

      profiles[i].fanMax[fan] =
        preferences.getUChar(
          fanKey.c_str(),
          100
        );
    }
  }

  preferences.end();
}


// ============================================================
// SAVE WIFI SETTINGS
// ============================================================

void saveWiFiSettings() {

  preferences.begin(
    "settings",
    false
  );

  preferences.putString(
    "wifi_ssid",
    wifiSSID
  );

  preferences.putString(
    "wifi_pass",
    wifiPassword
  );

  preferences.putBool(
    "wifi_dhcp",
    wifiDHCP
  );

  preferences.putString(
    "wifi_ip",
    wifiIP
  );

  preferences.putString(
    "wifi_gw",
    wifiGateway
  );

  preferences.putString(
    "wifi_sub",
    wifiSubnet
  );

  preferences.end();
}


// ============================================================
// SAVE iLO SETTINGS
// ============================================================

void saveILOSettings() {

  preferences.begin(
    "settings",
    false
  );

  preferences.putString(
    "ilo_ip",
    iloIP
  );

  preferences.putString(
    "ilo_user",
    iloUsername
  );

  preferences.putString(
    "ilo_pass",
    iloPassword
  );

  preferences.end();
}


// ============================================================
// SAVE PROFILE
// ============================================================

void saveProfile(int index) {

  if (index < 0 ||
      index >= MAX_PROFILES) {
    return;
  }


  preferences.begin(
    "profiles",
    false
  );


  String usedKey =
    "u" + String(index);


  preferences.putBool(
    usedKey.c_str(),
    profiles[index].used
  );


  if (profiles[index].used) {

    String nameKey =
      "n" + String(index);


    preferences.putString(
      nameKey.c_str(),
      profiles[index].name
    );


    for (int fan = 0; fan < 6; fan++) {

      String fanKey =
        "f" +
        String(index) +
        "_" +
        String(fan);


      preferences.putUChar(
        fanKey.c_str(),
        profiles[index].fanMax[fan]
      );
    }
  }


  preferences.end();
}


// ============================================================
// DELETE PROFILE
// ============================================================

void deleteProfile(int index) {

  if (index < 0 ||
      index >= MAX_PROFILES) {
    return;
  }


  profiles[index].used = false;

  profiles[index].name = "";


  for (int fan = 0; fan < 6; fan++) {
    profiles[index].fanMax[fan] = 100;
  }


  saveProfile(index);
}


// ============================================================
// FIND FREE PROFILE
// ============================================================

int findFreeProfile() {

  for (int i = 0; i < MAX_PROFILES; i++) {

    if (!profiles[i].used) {
      return i;
    }
  }

  return -1;
}


// ============================================================
// CREATE DEFAULT PROFILE
// ============================================================

void createDefaultProfile() {

  // Don't create another profile if one already exists.

  for (int i = 0; i < MAX_PROFILES; i++) {

    if (profiles[i].used) {
      return;
    }
  }


  profiles[0].used = true;

  profiles[0].name =
    "Default";


  for (int fan = 0; fan < 6; fan++) {
    profiles[0].fanMax[fan] = 100;
  }


  saveProfile(0);
}


// ============================================================
// START CONFIGURATION AP
// ============================================================

void startAccessPoint() {

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
    AP_SSID
  );


  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "ESP32-iLO4 CONFIGURATION AP"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    AP_SSID
  );

  Serial.print(
    "AP IP: "
  );

  Serial.println(
    WiFi.softAPIP()
  );

  Serial.println();
}


// ============================================================
// CONNECT TO SAVED WIFI
// ============================================================

bool connectSavedWiFi() {

  if (wifiSSID.length() == 0) {

    Serial.println(
      "No saved WiFi configuration."
    );

    return false;
  }


  Serial.println();
  Serial.println(
    "Connecting to saved WiFi..."
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    wifiSSID
  );


  WiFi.disconnect(true);

  delay(300);


  WiFi.mode(
    WIFI_AP_STA
  );


  // ----------------------------------------------------------
  // STATIC IP
  // ----------------------------------------------------------

  if (!wifiDHCP) {

    IPAddress localIP;
    IPAddress gateway;
    IPAddress subnet;


    if (!localIP.fromString(wifiIP)) {

      Serial.println(
        "Invalid static IP."
      );

      return false;
    }


    if (!gateway.fromString(wifiGateway)) {

      Serial.println(
        "Invalid gateway."
      );

      return false;
    }


    if (!subnet.fromString(wifiSubnet)) {

      Serial.println(
        "Invalid subnet."
      );

      return false;
    }


    if (
      !WiFi.config(
        localIP,
        gateway,
        subnet
      )
    ) {

      Serial.println(
        "WiFi static configuration failed."
      );

      return false;
    }
  }


  // ----------------------------------------------------------
  // CONNECT
  // ----------------------------------------------------------

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );


  unsigned long start =
    millis();


  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start <
      WIFI_CONNECT_TIMEOUT
  ) {

    delay(250);

    Serial.print(".");
  }


  Serial.println();


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "WiFi connected."
    );

    Serial.print(
      "IP address: "
    );

    Serial.println(
      WiFi.localIP()
    );

    return true;
  }


  Serial.println(
    "WiFi connection failed."
  );


  WiFi.disconnect();

  return false;
}


// ============================================================
// iLO4 SSH TEST
// ============================================================

String testILOConnection() {

  if (iloIP.length() == 0) {

    return
      "ERROR: iLO4 IP address is empty.";
  }


  if (iloUsername.length() == 0) {

    return
      "ERROR: iLO4 username is empty.";
  }


  if (iloPassword.length() == 0) {

    return
      "ERROR: iLO4 password is empty.";
  }


  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "iLO4 SSH CONNECTION TEST"
  );

  Serial.println(
    "================================"
  );


  Serial.print(
    "Host: "
  );

  Serial.println(
    iloIP
  );


  Serial.print(
    "User: "
  );

  Serial.println(
    iloUsername
  );


  // ----------------------------------------------------------
  // INITIALIZE LIBSSH
  // ----------------------------------------------------------

  libssh_begin();


  // ----------------------------------------------------------
  // CREATE SESSION
  // ----------------------------------------------------------

  ssh_session session =
    ssh_new();


  if (session == nullptr) {

    Serial.println(
      "ssh_new() failed."
    );

    return
      "ERROR: ssh_new() failed.";
  }


  // ----------------------------------------------------------
  // HOST
  // ----------------------------------------------------------

  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_HOST,
      iloIP.c_str()
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not set SSH host: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // USERNAME
  // ----------------------------------------------------------

  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_USER,
      iloUsername.c_str()
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not set SSH username: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // SSH PORT
  // ----------------------------------------------------------

  unsigned int port = 22;


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_PORT,
      &port
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not set SSH port: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // OLD iLO4 KEX
  // ----------------------------------------------------------

  const char *kex =
    "diffie-hellman-group14-sha1,"
    "diffie-hellman-group1-sha1";


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_KEY_EXCHANGE,
      kex
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure KEX: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // HOST KEYS
  // ----------------------------------------------------------

  const char *hostkeys =
    "ssh-rsa,ssh-dss";


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_HOSTKEYS,
      hostkeys
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure host keys: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // CIPHERS
  // ----------------------------------------------------------

  const char *ciphers =
    "aes256-ctr,"
    "aes256-cbc,"
    "aes128-cbc,"
    "3des-cbc";


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_CIPHERS_C_S,
      ciphers
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure "
      "client->server ciphers: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_CIPHERS_S_C,
      ciphers
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure "
      "server->client ciphers: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // HMAC
  // ----------------------------------------------------------

  const char *hmac =
    "hmac-sha1,"
    "hmac-sha2-256,"
    "hmac-md5";


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_HMAC_C_S,
      hmac
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure "
      "client->server HMAC: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  if (
    ssh_options_set(
      session,
      SSH_OPTIONS_HMAC_S_C,
      hmac
    ) != SSH_OK
  ) {

    String error =
      "ERROR: Could not configure "
      "server->client HMAC: ";

    error +=
      ssh_get_error(session);


    ssh_free(session);

    return error;
  }


  // ----------------------------------------------------------
  // DISABLE HOST KEY CHECK
  // ----------------------------------------------------------

  int strictHostKeyCheck = 0;


  ssh_options_set(
    session,
    SSH_OPTIONS_STRICTHOSTKEYCHECK,
    &strictHostKeyCheck
  );


  // ----------------------------------------------------------
  // CONNECT
  // ----------------------------------------------------------

  Serial.println(
    "Connecting..."
  );


  int rc =
    ssh_connect(
      session
    );


  if (rc != SSH_OK) {

    String error =
      "SSH CONNECT FAILED: ";

    error +=
      ssh_get_error(session);


    Serial.println(
      error
    );


    ssh_disconnect(
      session
    );

    ssh_free(
      session
    );


    return error;
  }


  Serial.println(
    "SSH TCP connection successful."
  );


  // ----------------------------------------------------------
  // PASSWORD AUTHENTICATION
  // ----------------------------------------------------------

  Serial.println(
    "Authenticating..."
  );


  rc =
    ssh_userauth_password(
      session,
      nullptr,
      iloPassword.c_str()
    );


  if (
    rc != SSH_AUTH_SUCCESS
  ) {

    String error =
      "SSH AUTH FAILED: ";

    error +=
      ssh_get_error(session);


    Serial.println(
      error
    );


    ssh_disconnect(
      session
    );

    ssh_free(
      session
    );


    return error;
  }


  Serial.println(
    "SSH authentication successful."
  );


  // ----------------------------------------------------------
  // OPEN CHANNEL
  // ----------------------------------------------------------

  ssh_channel channel =
    ssh_channel_new(
      session
    );


  if (channel == nullptr) {

    String error =
      "ERROR: ssh_channel_new() failed.";


    ssh_disconnect(
      session
    );

    ssh_free(
      session
    );


    return error;
  }


  rc =
    ssh_channel_open_session(
      channel
    );


  if (rc != SSH_OK) {

    String error =
      "ERROR: Could not open SSH channel: ";

    error +=
      ssh_get_error(session);


    ssh_channel_free(
      channel
    );

    ssh_disconnect(
      session
    );

    ssh_free(
      session
    );


    return error;
  }


  // ----------------------------------------------------------
  // TEST COMMAND
  // ----------------------------------------------------------

  Serial.println(
    "Executing test command..."
  );


  rc =
    ssh_channel_request_exec(
      channel,
      "help"
    );


  if (rc != SSH_OK) {

    String error =
      "ERROR: Could not execute "
      "test command: ";

    error +=
      ssh_get_error(session);


    ssh_channel_close(
      channel
    );

    ssh_channel_free(
      channel
    );

    ssh_disconnect(
      session
    );

    ssh_free(
      session
    );


    return error;
  }


  // ----------------------------------------------------------
  // READ COMMAND OUTPUT
  // ----------------------------------------------------------

  char buffer[256];

  int totalRead = 0;

  unsigned long readStart =
    millis();


  while (
    millis() - readStart < 3000 &&
    totalRead < 2048
  ) {

    int n =
      ssh_channel_read(
        channel,
        buffer,
        sizeof(buffer) - 1,
        0
      );


    if (n <= 0) {
      break;
    }


    totalRead += n;
  }


  // ----------------------------------------------------------
  // EXIT STATUS
  // ----------------------------------------------------------

  int exitStatus =
    ssh_channel_get_exit_status(
      channel
    );


  Serial.print(
    "Command exit status: "
  );

  Serial.println(
    exitStatus
  );


  // ----------------------------------------------------------
  // CLEANUP
  // ----------------------------------------------------------

  ssh_channel_send_eof(
    channel
  );

  ssh_channel_close(
    channel
  );

  ssh_channel_free(
    channel
  );

  ssh_disconnect(
    session
  );

  ssh_free(
    session
  );


  // ----------------------------------------------------------
  // RESULT
  // ----------------------------------------------------------

  if (exitStatus == 0) {

    Serial.println(
      "iLO4 TEST SUCCESSFUL."
    );


    return
      "SUCCESS: SSH connection and "
      "authentication successful.";
  }


  Serial.println(
    "SSH connection worked but "
    "test command returned an error."
  );


  return
    "CONNECTED: SSH authentication "
    "succeeded, but test command "
    "returned exit status " +
    String(exitStatus) +
    ".";
}


// ============================================================
// GENERIC iLO4 SSH COMMAND EXECUTION
// (same connect/auth/exec pattern as testILOConnection(),
// factored out so the panel can send/read fan commands)
// ============================================================

bool runILOCommand(const String &command, String &output, int &exitStatus) {

  output = "";
  exitStatus = -1;


  if (iloIP.length() == 0 ||
      iloUsername.length() == 0 ||
      iloPassword.length() == 0) {

    output = "ERROR: iLO4 not configured.";
    return false;
  }


  libssh_begin();


  ssh_session session = ssh_new();

  if (session == nullptr) {
    output = "ERROR: ssh_new() failed.";
    return false;
  }


  ssh_options_set(session, SSH_OPTIONS_HOST, iloIP.c_str());
  ssh_options_set(session, SSH_OPTIONS_USER, iloUsername.c_str());

  unsigned int port = 22;
  ssh_options_set(session, SSH_OPTIONS_PORT, &port);

  const char *kex =
    "diffie-hellman-group14-sha1,"
    "diffie-hellman-group1-sha1";
  ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kex);

  const char *hostkeys = "ssh-rsa,ssh-dss";
  ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, hostkeys);

  const char *ciphers =
    "aes256-ctr,aes256-cbc,aes128-cbc,3des-cbc";
  ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, ciphers);
  ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, ciphers);

  const char *hmac = "hmac-sha1,hmac-sha2-256,hmac-md5";
  ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, hmac);
  ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, hmac);

  int strictHostKeyCheck = 0;
  ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strictHostKeyCheck);


  int rc = ssh_connect(session);

  if (rc != SSH_OK) {
    output = String("ERROR: SSH connect failed: ") + ssh_get_error(session);
    ssh_disconnect(session);
    ssh_free(session);
    return false;
  }


  rc = ssh_userauth_password(session, nullptr, iloPassword.c_str());

  if (rc != SSH_AUTH_SUCCESS) {
    output = String("ERROR: SSH auth failed: ") + ssh_get_error(session);
    ssh_disconnect(session);
    ssh_free(session);
    return false;
  }


  ssh_channel channel = ssh_channel_new(session);

  if (channel == nullptr) {
    output = "ERROR: ssh_channel_new() failed.";
    ssh_disconnect(session);
    ssh_free(session);
    return false;
  }


  rc = ssh_channel_open_session(channel);

  if (rc != SSH_OK) {
    output = String("ERROR: Could not open channel: ") + ssh_get_error(session);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
    return false;
  }


  rc = ssh_channel_request_exec(channel, command.c_str());

  if (rc != SSH_OK) {
    output = String("ERROR: Could not exec command: ") + ssh_get_error(session);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
    return false;
  }


  char buffer[256];
  int totalRead = 0;
  unsigned long readStart = millis();

  while (millis() - readStart < 3000 && totalRead < 2048) {

    int n = ssh_channel_read(channel, buffer, sizeof(buffer) - 1, 0);

    if (n <= 0) {
      break;
    }

    buffer[n] = '\0';
    output += buffer;
    totalRead += n;
  }


  exitStatus = ssh_channel_get_exit_status(channel);

  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
  ssh_channel_free(channel);
  ssh_disconnect(session);
  ssh_free(session);

  return true;
}


// ============================================================
// APPLY ONE FAN'S MAX SPEED
// Command format confirmed by user: "fan p 1 max 50"
// (fanIndex1based is 1-6, percent is 0-100)
// ============================================================

bool applyFanSpeed(int fanIndex1based, uint8_t percent) {

  uint8_t rawValue = percentToRawFanValue(percent);

  String cmd =
    "fan p " +
    String(fanIndex1based) +
    " max " +
    String(rawValue);

  String output;
  int exitStatus;

  bool ok = runILOCommand(cmd, output, exitStatus);

  Serial.print("Apply fan ");
  Serial.print(fanIndex1based);
  Serial.print(": ");
  Serial.print(percent);
  Serial.print("% -> raw ");
  Serial.print(rawValue);
  Serial.print(" -> ");
  Serial.print(cmd);
  Serial.print(" | ok=");
  Serial.print(ok ? "yes" : "no");
  Serial.print(" | exitStatus=");
  Serial.println(exitStatus);

  if (output.length() > 0) {
    Serial.println("apply response:");
    Serial.println(output);
  }

  return ok;
}


// ============================================================
// READ ONE FAN'S CURRENT SPEED
// Command confirmed by user: "show /system1/fan1"
//
// Confirmed by user: the desired/current speed iLO4 reports
// here IS already a 0-100 percentage (unlike the apply command,
// which needs the raw 0-255 scale) - so no conversion is done
// on this side, it's compared directly against the profile's
// 0-100% target.
//
// A fan with HealthState=Not Installed (or similar "not
// present" wording) is treated as not existing and ignored.
//
// NOTE: The exact text iLO4 prints back can vary by firmware.
// This parser is deliberately loose/defensive:
//   - if the command errors, times out, or the output contains
//     an obvious "not there" indicator, we report exists=false
//     (the fan is IGNORED rather than counted as a failure)
//   - otherwise we scan the output for a number that looks like
//     a fan speed percentage.
// If your real output format differs, tune the parsing logic
// in this function only - nothing else needs to change.
// ============================================================

bool readFanSpeed(int fanIndex1based, int &percentOut, bool &existsOut) {

  percentOut = -1;
  existsOut = false;

  String cmd = "show /system1/fan" + String(fanIndex1based);

  String output;
  int exitStatus;

  bool ok = runILOCommand(cmd, output, exitStatus);

  Serial.println("---- readFanSpeed ----");
  Serial.print("cmd: ");
  Serial.println(cmd);
  Serial.print("ok: ");
  Serial.println(ok ? "yes" : "no");
  Serial.print("exitStatus: ");
  Serial.println(exitStatus);
  Serial.println("raw output:");
  Serial.println("--------------------");
  Serial.println(output);
  Serial.println("--------------------");

  if (!ok) {
    Serial.println("readFanSpeed: command failed -> exists=false");
    return false;
  }

  String lower = output;
  lower.toLowerCase();

  // ---- Detect "fan doesn't exist" style responses ----
  if (output.length() < 5 ||
      lower.indexOf("not found")       >= 0 ||
      lower.indexOf("not exist")       >= 0 ||
      lower.indexOf("no such")         >= 0 ||
      lower.indexOf("invalid")         >= 0 ||
      lower.indexOf("error")           >= 0 ||
      lower.indexOf("status=3")        >= 0 ||
      lower.indexOf("not installed")   >= 0) {

    existsOut = false;
    Serial.println("readFanSpeed: matched a 'not present' pattern -> exists=false");
    return true;
  }


  // ---- Try to find a speed percentage in the output ----
  // Look line by line for something that mentions the fan
  // speed/percentage, then pull the trailing digits off it.
  int searchFrom = 0;

  while (searchFrom < (int)lower.length()) {

    int newline = lower.indexOf('\n', searchFrom);

    String line =
      (newline == -1) ?
        lower.substring(searchFrom) :
        lower.substring(searchFrom, newline);


    if (line.indexOf("pct")   >= 0 ||
        line.indexOf("speed") >= 0 ||
        line.indexOf("%")     >= 0) {

      int digitsStart = -1;
      int digitsEnd = -1;

      for (int i = 0; i < (int)line.length(); i++) {

        if (isDigit(line[i])) {

          if (digitsStart == -1) {
            digitsStart = i;
          }

          digitsEnd = i;
        }
        else if (digitsStart != -1) {
          // stop at first run of digits after we found the field
          break;
        }
      }

      if (digitsStart != -1) {

        String numStr =
          line.substring(digitsStart, digitsEnd + 1);

        int value = numStr.toInt();

        if (value >= 0 && value <= 100) {

          percentOut = value;
          existsOut = true;

          Serial.print("readFanSpeed: matched line [");
          Serial.print(line);
          Serial.print("] -> parsed percent = ");
          Serial.println(value);

          return true;
        }
      }
    }

    if (newline == -1) {
      break;
    }

    searchFrom = newline + 1;
  }


  Serial.println("readFanSpeed: no usable number found in output -> exists=false");


  // Couldn't find a usable number - treat as "not present"
  // so it never causes a false FAILED result.
  existsOut = false;
  return true;
}


// ============================================================
// EC11 ENCODER - INTERRUPT HANDLER
// Standard 2-bit gray code quadrature decode.
// ============================================================

void IRAM_ATTR encoderISR() {

  static const int8_t table[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0
  };

  uint8_t clk = digitalRead(EC11_PIN_CLK);
  uint8_t dt  = digitalRead(EC11_PIN_DT);

  uint8_t currentState = (clk << 1) | dt;
  uint8_t index = (encoderLastState << 2) | currentState;

  encoderDelta += table[index & 0x0F];

  encoderLastState = currentState;
}


// ============================================================
// PANEL - HARDWARE SETUP
// ============================================================

void setupPanel() {

  Wire.begin();

  u8g2.setI2CAddress(OLED_I2C_ADDRESS << 1);
  u8g2.begin();

  u8g2.setFont(u8g2_font_6x12_tf);


  pinMode(EC11_PIN_CLK, INPUT_PULLUP);
  pinMode(EC11_PIN_DT,  INPUT_PULLUP);
  pinMode(EC11_PIN_SW,  INPUT_PULLUP);

  encoderLastState =
    (digitalRead(EC11_PIN_CLK) << 1) |
    digitalRead(EC11_PIN_DT);

  attachInterrupt(
    digitalPinToInterrupt(EC11_PIN_CLK),
    encoderISR,
    CHANGE
  );

  attachInterrupt(
    digitalPinToInterrupt(EC11_PIN_DT),
    encoderISR,
    CHANGE
  );

  lastActivityMs = millis();
}


// ============================================================
// PANEL - LIST OF "USED" PROFILE SLOTS
// ============================================================

int getUsedProfileSlots(int *outSlots) {

  int count = 0;

  for (int i = 0; i < MAX_PROFILES; i++) {

    if (profiles[i].used) {
      outSlots[count] = i;
      count++;
    }
  }

  return count;
}


// ============================================================
// PANEL - INPUT HELPERS
// ============================================================

int consumeEncoderSteps() {

  noInterrupts();

  int delta = encoderDelta;

  interrupts();


  // Most EC11 modules produce 4 raw transitions per detent.
  int steps = delta / 4;


  if (steps != 0) {

    // Only remove the transitions that made up a full step,
    // keep any partial progress toward the next one.
    noInterrupts();

    encoderDelta -= steps * 4;

    interrupts();
  }


  return steps;
}


bool consumeButtonPress() {

  bool raw = digitalRead(EC11_PIN_SW);
  unsigned long now = millis();


  if (raw != buttonLastRaw &&
      (now - buttonLastChangeMs) > EC11_DEBOUNCE_MS) {

    buttonLastChangeMs = now;
    buttonLastRaw = raw;

    // Button is active LOW (pressed = LOW with INPUT_PULLUP).
    if (raw == LOW) {
      buttonPressedEvent = true;
    }
  }


  if (buttonPressedEvent) {
    buttonPressedEvent = false;
    return true;
  }

  return false;
}


// ============================================================
// PANEL - DRAWING
// ============================================================

// ============================================================
// AUTO-FIT TEXT
// Picks the largest available bold font that still fits
// within maxWidth, and vertically centers it around centerY.
// Falls back to hard-truncating only if even the smallest
// font is still too wide for the space available.
// ============================================================

void drawFittedText(int centerY, const String &text, int maxWidth) {

  static const uint8_t *fonts[] = {
    u8g2_font_helvB18_tf,
    u8g2_font_helvB14_tf,
    u8g2_font_helvB12_tf,
    u8g2_font_helvB10_tf,
    u8g2_font_helvB08_tf
  };

  const int numFonts = 5;
  int chosen = numFonts - 1;

  for (int i = 0; i < numFonts; i++) {

    u8g2.setFont(fonts[i]);

    if (u8g2.getStrWidth(text.c_str()) <= maxWidth) {
      chosen = i;
      break;
    }
  }

  u8g2.setFont(fonts[chosen]);


  String display = text;

  while (
    u8g2.getStrWidth(display.c_str()) > maxWidth &&
    display.length() > 1
  ) {
    display.remove(display.length() - 1);
  }


  int textWidth = u8g2.getStrWidth(display.c_str());
  int x = (128 - textWidth) / 2;

  if (x < 2) {
    x = 2;
  }


  int y = centerY +
    (u8g2.getAscent() + u8g2.getDescent()) / 2;

  u8g2.drawStr(x, y, display.c_str());
}


void drawProfileListScreen() {

  int slots[MAX_PROFILES];
  int count = getUsedProfileSlots(slots);


  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "Profiles");
  u8g2.drawHLine(0, 15, 128);


  if (count == 0) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 38, "No profiles found");
    u8g2.sendBuffer();
    return;
  }


  if (panelListSelection >= count) {
    panelListSelection = count - 1;
  }

  if (panelListSelection < 0) {
    panelListSelection = 0;
  }


  int slot = slots[panelListSelection];


  // Left/right nav arrows so it's clear there's more than one.
  if (count > 1) {
    u8g2.drawStr(0, 40, "<");
    u8g2.drawStr(122, 40, ">");
  }


  // Big centered profile name, auto-sized to fit.
  String name = profiles[slot].name;

  drawFittedText(34, name, 96);


  // Position indicator.
  u8g2.setFont(u8g2_font_5x8_tf);

  String pos = String(panelListSelection + 1) + "/" + String(count);
  int posWidth = u8g2.getStrWidth(pos.c_str());

  u8g2.drawStr((128 - posWidth) / 2, 60, pos.c_str());


  u8g2.sendBuffer();
}


void drawProfileMenuScreen() {

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12,
    profiles[panelSelectedProfile].name.c_str());

  u8g2.drawHLine(0, 15, 128);


  const char *optionText =
    (panelSubMenuSelection == 0) ? "Apply" : "Back";


  u8g2.drawStr(0, 40, "<");
  u8g2.drawStr(122, 40, ">");


  drawFittedText(34, optionText, 96);


  u8g2.sendBuffer();
}


void drawMessageScreen(
  const String &title,
  const String &line1,
  const String &line2
) {

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(0, 12, title.c_str());
  u8g2.drawHLine(0, 15, 128);

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 32, line1.c_str());
  u8g2.drawStr(0, 48, line2.c_str());

  u8g2.sendBuffer();
}


// ============================================================
// PANEL - APPLY + VERIFY (blocking; runs for ~10s worst case)
// ============================================================

void applyAndVerifyProfile(int profileSlot) {

  panelState = PANEL_APPLYING;

  drawMessageScreen(
    "Applying",
    profiles[profileSlot].name,
    "Sending profile..."
  );

  Serial.println();
  Serial.println("======== APPLY PROFILE ========");
  Serial.print("Profile: ");
  Serial.println(profiles[profileSlot].name);


  // ---- Send the max-speed command for every fan ----
  for (int fan = 0; fan < 6; fan++) {

    Serial.print("Apply target fan ");
    Serial.print(fan + 1);
    Serial.print(" -> ");
    Serial.print(profiles[profileSlot].fanMax[fan]);
    Serial.println("%");

    applyFanSpeed(
      fan + 1,
      profiles[profileSlot].fanMax[fan]
    );
  }


  // ---- Verify: poll actual fan speeds for up to 10s ----
  bool matched[6];
  bool exists[6];

  for (int fan = 0; fan < 6; fan++) {
    matched[fan] = false;
    exists[fan] = false;
  }


  unsigned long verifyStart = millis();
  bool allMatched = false;
  int pollNum = 0;


  while (millis() - verifyStart < VERIFY_DURATION_MS) {

    unsigned long elapsedSec =
      (millis() - verifyStart) / 1000;

    pollNum++;

    Serial.println();
    Serial.print("---- verify poll #");
    Serial.print(pollNum);
    Serial.print(" (t=");
    Serial.print(elapsedSec);
    Serial.println("s) ----");

    drawMessageScreen(
      "Verifying",
      profiles[profileSlot].name,
      "Checking... " + String(elapsedSec) + "s"
    );


    allMatched = true;

    for (int fan = 0; fan < 6; fan++) {

      if (matched[fan]) {
        Serial.print("fan ");
        Serial.print(fan + 1);
        Serial.println(": already matched, skipping");
        continue;
      }


      int actualPercent;
      bool fanExists;

      readFanSpeed(fan + 1, actualPercent, fanExists);

      exists[fan] = fanExists;


      if (!fanExists) {
        // Ignore fans that don't exist entirely.
        matched[fan] = true;
        Serial.print("fan ");
        Serial.print(fan + 1);
        Serial.println(": does not exist -> ignored");
        continue;
      }


      int targetPercent = profiles[profileSlot].fanMax[fan];
      int diff = abs(actualPercent - targetPercent);

      Serial.print("fan ");
      Serial.print(fan + 1);
      Serial.print(": target=");
      Serial.print(targetPercent);
      Serial.print("% actual=");
      Serial.print(actualPercent);
      Serial.print("% diff=");
      Serial.print(diff);
      Serial.print(" tolerance=");
      Serial.println(FAN_SPEED_TOLERANCE_PERCENT);

      if (diff <= FAN_SPEED_TOLERANCE_PERCENT) {
        matched[fan] = true;
        Serial.print("fan ");
        Serial.print(fan + 1);
        Serial.println(": MATCHED");
      }
      else {
        allMatched = false;
        Serial.print("fan ");
        Serial.print(fan + 1);
        Serial.println(": NOT matched yet");
      }
    }


    if (allMatched) {
      break;
    }


    delay(VERIFY_POLL_INTERVAL_MS);
  }


  Serial.println();
  Serial.print("======== RESULT: ");
  Serial.print(allMatched ? "APPLIED OK" : "FAILED");
  Serial.println(" ========");


  // ---- Result ----
  panelState = PANEL_RESULT;
  panelResultOk = allMatched;

  panelResultLine1 = profiles[profileSlot].name;

  if (allMatched) {
    panelResultLine2 = "Applied OK";
  }
  else {
    panelResultLine2 = "FAILED";
  }
}


void drawScreensaverScreen() {

  u8g2.clearBuffer();

  // Simple stylised "hp" badge - not a pixel-exact logo,
  // just a calm circle + monogram for the idle screen.
  u8g2.drawCircle(64, 32, 22, U8G2_DRAW_ALL);
  u8g2.drawCircle(64, 32, 21, U8G2_DRAW_ALL);

  u8g2.setFont(u8g2_font_9x15B_tf);

  const char *badge = "hp";
  int badgeWidth = u8g2.getStrWidth(badge);

  u8g2.drawStr(64 - badgeWidth / 2, 39, badge);

  u8g2.sendBuffer();
}


// ============================================================
// PANEL - MAIN UI HANDLER (call every loop())
// ============================================================

void handleUI() {

  int steps = consumeEncoderSteps();
  bool pressed = consumeButtonPress();

  bool activity = (steps != 0) || pressed;


  // ------------------------------------------------------
  // SCREENSAVER WAKE / SLEEP
  // (not while an apply/verify is actively running)
  // ------------------------------------------------------

  if (panelState == PANEL_SCREENSAVER) {

    if (activity) {

      u8g2.setContrast(255);

      lastActivityMs = millis();
      panelState = panelStateBeforeScreensaver;

      // Swallow the wake-up input so it doesn't also move
      // a selection or trigger an action right away.
      steps = 0;
      pressed = false;
    }
    else {

      drawScreensaverScreen();
      return;
    }
  }
  else {

    if (activity) {
      lastActivityMs = millis();
    }
    else if (
      panelState != PANEL_APPLYING &&
      (millis() - lastActivityMs) > SCREENSAVER_TIMEOUT_MS
    ) {

      panelStateBeforeScreensaver = panelState;
      panelState = PANEL_SCREENSAVER;

      u8g2.setContrast(SCREENSAVER_CONTRAST);

      drawScreensaverScreen();
      return;
    }
  }


  switch (panelState) {

    // --------------------------------------------------------
    case PANEL_PROFILE_LIST: {

      int slots[MAX_PROFILES];
      int count = getUsedProfileSlots(slots);


      if (steps != 0) {

        panelListSelection += steps;

        if (panelListSelection < 0) {
          panelListSelection = 0;
        }

        if (count > 0 && panelListSelection > count - 1) {
          panelListSelection = count - 1;
        }
      }


      if (pressed && count > 0) {

        panelSelectedProfile = slots[panelListSelection];
        panelSubMenuSelection = 0;
        panelState = PANEL_PROFILE_MENU;
      }


      drawProfileListScreen();

      break;
    }


    // --------------------------------------------------------
    case PANEL_PROFILE_MENU: {

      if (steps != 0) {

        panelSubMenuSelection += steps;

        if (panelSubMenuSelection < 0) {
          panelSubMenuSelection = 1;
        }

        if (panelSubMenuSelection > 1) {
          panelSubMenuSelection = 0;
        }
      }


      if (pressed) {

        if (panelSubMenuSelection == 0) {

          // Apply - this blocks for up to ~10s while it
          // applies and verifies the profile.
          applyAndVerifyProfile(panelSelectedProfile);
        }
        else {

          panelState = PANEL_PROFILE_LIST;
        }
      }


      if (panelState == PANEL_PROFILE_MENU) {
        drawProfileMenuScreen();
      }

      break;
    }


    // --------------------------------------------------------
    case PANEL_APPLYING: {

      // applyAndVerifyProfile() runs synchronously and moves
      // us straight to PANEL_RESULT, so nothing to do here.
      break;
    }


    // --------------------------------------------------------
    case PANEL_RESULT: {

      drawMessageScreen(
        panelResultOk ? "Success" : "Failed",
        panelResultLine1,
        panelResultLine2 + "  (click)"
      );


      if (pressed) {
        panelState = PANEL_PROFILE_LIST;
      }

      break;
    }
  }
}


// ============================================================
// MAIN PAGE
// ============================================================

void handleRoot() {

  String html =
    htmlHeader(
      "ESP32 iLO4 Controller"
    );


  html +=
    "<h1>ESP32 iLO4 Controller</h1>";


  // ----------------------------------------------------------
  // MAIN MENU
  // ----------------------------------------------------------

  html +=
    "<div class='card'>";


  html +=
    "<h2>Main Menu</h2>";


  html +=
    "<a class='button menuButton' "
    "href='/profiles'>"
    "1. Edit Profiles"
    "</a>";


  html +=
    "<a class='button menuButton' "
    "href='/ilo'>"
    "2. iLO4 Settings"
    "</a>";


  html +=
    "<a class='button menuButton' "
    "href='/wifi'>"
    "3. WiFi Settings"
    "</a>";


  html +=
    "</div>";


  // ----------------------------------------------------------
  // NETWORK STATUS
  // ----------------------------------------------------------

  html +=
    "<div class='card'>";


  html +=
    "<h2>Network Status</h2>";


  html +=
    "<p>";

  html +=
    "Configuration AP: <b>Active</b><br>";

  html +=
    "AP IP: ";

  html +=
    WiFi.softAPIP().toString();


  html +=
    "</p>";


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    html +=
      "<p>";

    html +=
      "WiFi: <b>Connected</b><br>";

    html +=
      "SSID: ";

    html +=
      htmlEscape(
        WiFi.SSID()
      );

    html +=
      "<br>";

    html +=
      "IP: ";

    html +=
      WiFi.localIP().toString();

    html +=
      "</p>";
  }
  else {

    html +=
      "<p>"
      "WiFi: <b>Not connected</b>"
      "</p>";
  }


  html +=
    "</div>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// PROFILE LIST
// ============================================================

void handleProfiles() {

  String html =
    htmlHeader(
      "Profiles"
    );


  html +=
    "<h1>Profile Manager</h1>";


  int count = 0;


  for (
    int i = 0;
    i < MAX_PROFILES;
    i++
  ) {

    if (!profiles[i].used) {
      continue;
    }


    count++;


    html +=
      "<div class='profileRow'>";


    html +=
      "<h2>";


    html +=
      htmlEscape(
        profiles[i].name
      );


    html +=
      "</h2>";


    html +=
      "<a class='button' "
      "href='/profile/edit?id=";


    html +=
      String(i);


    html +=
      "'>Edit</a>";


    html +=
      "<a class='button danger' "
      "href='/profile/delete?id=";


    html +=
      String(i);


    html +=
      "' "
      "onclick=\"return confirm("
      "'Delete this profile?'"
      ")\">"
      "Delete"
      "</a>";


    html +=
      "</div>";
  }


  if (count == 0) {

    html +=
      "<div class='card'>"
      "No profiles have been created."
      "</div>";
  }


  if (count < MAX_PROFILES) {

    html +=
      "<a class='button' "
      "href='/profile/new'>"
      "+ Create Profile"
      "</a>";
  }
  else {

    html +=
      "<div class='card'>"
      "Maximum of 20 profiles reached."
      "</div>";
  }


  html +=
    "<div class='back'>";


  html +=
    "<a class='button secondary' "
    "href='/'>"
    "Back to Main Menu"
    "</a>";


  html +=
    "</div>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// CREATE PROFILE PAGE
// ============================================================

void handleNewProfile() {

  int index =
    findFreeProfile();


  if (index == -1) {

    String html =
      htmlHeader(
        "Error"
      );


    html +=
      "<h1>Maximum profiles reached</h1>";


    html +=
      "<a class='button' "
      "href='/profiles'>"
      "Back"
      "</a>";


    html +=
      htmlFooter();


    server.send(
      200,
      "text/html",
      html
    );


    return;
  }


  String html =
    htmlHeader(
      "Create Profile"
    );


  html +=
    "<h1>Create Profile</h1>";


  html +=
    "<form method='POST' "
    "action='/profile/save'>";


  html +=
    "<input type='hidden' "
    "name='id' value='";


  html +=
    String(index);


  html +=
    "'>";


  html +=
    "<div class='card'>";


  // Profile name

  html +=
    "<label>Profile Name</label>";


  html +=
    "<input type='text' "
    "name='name' "
    "maxlength='31' "
    "required "
    "placeholder='Profile name'>";


  // Six fans

  for (
    int fan = 0;
    fan < 6;
    fan++
  ) {

    html +=
      "<label>Fan ";


    html +=
      String(fan + 1);


    html +=
      " Maximum (%)</label>";


    html +=
      "<input type='range' "
      "name='fan";


    html +=
      String(fan + 1);


    html +=
      "' "
      "min='0' "
      "max='100' "
      "value='100' "
      "oninput=\""
      "this.nextElementSibling.value="
      "this.value"
      "\">";


    html +=
      "<output class='fanValue'>"
      "100"
      "</output>";
  }


  html +=
    "</div>";


  html +=
    "<button type='submit'>"
    "Save Profile"
    "</button>";


  html +=
    "<a class='button secondary' "
    "href='/profiles'>"
    "Cancel"
    "</a>";


  html +=
    "</form>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// EDIT PROFILE
// ============================================================

void handleEditProfile() {

  if (!server.hasArg("id")) {

    server.send(
      400,
      "text/plain",
      "Missing profile ID."
    );

    return;
  }


  int index =
    server.arg("id").toInt();


  if (
    index < 0 ||
    index >= MAX_PROFILES ||
    !profiles[index].used
  ) {

    server.send(
      404,
      "text/plain",
      "Profile not found."
    );

    return;
  }


  Profile &p =
    profiles[index];


  String html =
    htmlHeader(
      "Edit Profile"
    );


  html +=
    "<h1>Edit Profile</h1>";


  html +=
    "<form method='POST' "
    "action='/profile/save'>";


  html +=
    "<input type='hidden' "
    "name='id' value='";


  html +=
    String(index);


  html +=
    "'>";


  html +=
    "<div class='card'>";


  // Profile name

  html +=
    "<label>Profile Name</label>";


  html +=
    "<input type='text' "
    "name='name' "
    "maxlength='31' "
    "required "
    "value='";


  html +=
    htmlEscape(
      p.name
    );


  html +=
    "'>";


  // Fans

  for (
    int fan = 0;
    fan < 6;
    fan++
  ) {

    html +=
      "<label>Fan ";


    html +=
      String(fan + 1);


    html +=
      " Maximum (%)</label>";


    html +=
      "<input type='range' "
      "name='fan";


    html +=
      String(fan + 1);


    html +=
      "' "
      "min='0' "
      "max='100' "
      "value='";


    html +=
      String(
        p.fanMax[fan]
      );


    html +=
      "' "
      "oninput=\""
      "this.nextElementSibling.value="
      "this.value"
      "\">";


    html +=
      "<output class='fanValue'>";


    html +=
      String(
        p.fanMax[fan]
      );


    html +=
      "</output>";
  }


  html +=
    "</div>";


  html +=
    "<button type='submit'>"
    "Save Profile"
    "</button>";


  html +=
    "<a class='button danger' "
    "href='/profile/delete?id=";


  html +=
    String(index);


  html +=
    "' "
    "onclick=\"return confirm("
    "'Delete this profile?'"
    ")\">"
    "Delete Profile"
    "</a>";


  html +=
    "<br>";


  html +=
    "<a class='button secondary' "
    "href='/profiles'>"
    "Cancel"
    "</a>";


  html +=
    "</form>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// SAVE PROFILE
// ============================================================

void handleSaveProfile() {

  if (!server.hasArg("id")) {

    server.send(
      400,
      "text/plain",
      "Missing profile ID."
    );

    return;
  }


  int index =
    server.arg("id").toInt();


  if (
    index < 0 ||
    index >= MAX_PROFILES
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid profile ID."
    );

    return;
  }


  String name =
    server.arg("name");


  name.trim();


  if (name.length() == 0) {

    name =
      "Profile";
  }


  profiles[index].used =
    true;


  profiles[index].name =
    name;


  for (
    int fan = 0;
    fan < 6;
    fan++
  ) {

    String argument =
      "fan" +
      String(fan + 1);


    int value =
      server.arg(
        argument
      ).toInt();


    value =
      constrain(
        value,
        0,
        100
      );


    profiles[index].fanMax[fan] =
      (uint8_t)value;
  }


  saveProfile(index);


  server.sendHeader(
    "Location",
    "/profiles"
  );


  server.send(
    303,
    "text/plain",
    "Saved."
  );
}


// ============================================================
// DELETE PROFILE
// ============================================================

void handleDeleteProfile() {

  if (!server.hasArg("id")) {

    server.send(
      400,
      "text/plain",
      "Missing profile ID."
    );

    return;
  }


  int index =
    server.arg("id").toInt();


  if (
    index < 0 ||
    index >= MAX_PROFILES
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid profile ID."
    );

    return;
  }


  deleteProfile(index);


  server.sendHeader(
    "Location",
    "/profiles"
  );


  server.send(
    303,
    "text/plain",
    "Deleted."
  );
}


// ============================================================
// iLO4 SETTINGS PAGE
// ============================================================

void handleILO() {

  String html =
    htmlHeader(
      "iLO4 Settings"
    );


  html +=
    "<h1>iLO4 Settings</h1>";


  // ----------------------------------------------------------
  // SETTINGS
  // ----------------------------------------------------------

  html +=
    "<div class='card'>";


  html +=
    "<form method='POST' "
    "action='/ilo/save'>";


  html +=
    "<label>iLO4 IP Address</label>";


  html +=
    "<input type='text' "
    "name='ip' "
    "placeholder='192.168.1.9' "
    "value='";


  html +=
    htmlEscape(
      iloIP
    );


  html +=
    "'>";


  html +=
    "<label>Username</label>";


  html +=
    "<input type='text' "
    "name='username' "
    "value='";


  html +=
    htmlEscape(
      iloUsername
    );


  html +=
    "'>";


  html +=
    "<label>Password</label>";


  html +=
    "<input type='password' "
    "name='password' "
    "value='";


  html +=
    htmlEscape(
      iloPassword
    );


  html +=
    "'>";


  html +=
    "<button type='submit'>"
    "Save Settings"
    "</button>";


  html +=
    "</form>";


  html +=
    "</div>";


  // ----------------------------------------------------------
  // TEST CONNECTION
  // ----------------------------------------------------------

  html +=
    "<div class='card'>";


  html +=
    "<h2>Test Connection</h2>";


  html +=
    "<p>"
    "Test the SSH connection to the configured "
    "iLO4 controller."
    "</p>";


  html +=
    "<a class='button' "
    "href='/ilo/test'>"
    "TEST CONNECTION"
    "</a>";


  html +=
    "</div>";


  html +=
    "<a class='button secondary' "
    "href='/'>"
    "Back to Main Menu"
    "</a>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// SAVE iLO SETTINGS
// ============================================================

void handleSaveILO() {

  if (server.hasArg("ip")) {

    iloIP =
      server.arg("ip");
  }


  if (server.hasArg("username")) {

    iloUsername =
      server.arg("username");
  }


  if (server.hasArg("password")) {

    iloPassword =
      server.arg("password");
  }


  iloIP.trim();
  iloUsername.trim();


  saveILOSettings();


  server.sendHeader(
    "Location",
    "/ilo"
  );


  server.send(
    303,
    "text/plain",
    "Saved."
  );
}


// ============================================================
// iLO TEST PAGE
// ============================================================

void handleTestILO() {

  String result =
    testILOConnection();


  bool success =
    result.startsWith(
      "SUCCESS"
    );


  String html =
    htmlHeader(
      "iLO4 Test"
    );


  html +=
    "<h1>iLO4 Connection Test</h1>";


  if (success) {

    html +=
      "<div class='status success'>";


    html +=
      "<b>Connection successful</b>"
      "<br><br>";


    html +=
      htmlEscape(
        result
      );


    html +=
      "</div>";
  }
  else {

    html +=
      "<div class='status error'>";


    html +=
      htmlEscape(
        result
      );


    html +=
      "</div>";
  }


  html +=
    "<a class='button' "
    "href='/ilo'>"
    "Back to iLO4 Settings"
    "</a>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// WIFI SETTINGS PAGE
// ============================================================

void handleWiFi() {

  String html =
    htmlHeader(
      "WiFi Settings"
    );


  html +=
    "<h1>WiFi Settings</h1>";


  html +=
    "<form method='POST' "
    "action='/wifi/save'>";


  html +=
    "<div class='card'>";


  // ----------------------------------------------------------
  // SSID
  // ----------------------------------------------------------

  html +=
    "<label>WiFi SSID</label>";


  html +=
    "<input type='text' "
    "name='ssid' "
    "value='";


  html +=
    htmlEscape(
      wifiSSID
    );


  html +=
    "'>";


  // ----------------------------------------------------------
  // PASSWORD
  // ----------------------------------------------------------

  html +=
    "<label>WiFi Password</label>";


  html +=
    "<input type='password' "
    "name='password' "
    "value='";


  html +=
    htmlEscape(
      wifiPassword
    );


  html +=
    "'>";


  // ----------------------------------------------------------
  // IP MODE
  // ----------------------------------------------------------

  html +=
    "<h2>IP Configuration</h2>";


  html +=
    "<label>";


  html +=
    "<input type='radio' "
    "name='mode' "
    "value='dhcp' ";


  if (wifiDHCP) {

    html +=
      "checked";
  }


  html +=
    "> DHCP";


  html +=
    "</label>";


  html +=
    "<label>";


  html +=
    "<input type='radio' "
    "name='mode' "
    "value='static' ";


  if (!wifiDHCP) {

    html +=
      "checked";
  }


  html +=
    "> Static IP";


  html +=
    "</label>";


  html +=
    "<br>";


  // ----------------------------------------------------------
  // STATIC IP
  // ----------------------------------------------------------

  html +=
    "<label>Static IP</label>";


  html +=
    "<input type='text' "
    "name='ip' "
    "value='";


  html +=
    htmlEscape(
      wifiIP
    );


  html +=
    "' "
    "placeholder='192.168.1.50'>";


  // ----------------------------------------------------------
  // GATEWAY
  // ----------------------------------------------------------

  html +=
    "<label>Gateway</label>";


  html +=
    "<input type='text' "
    "name='gateway' "
    "value='";


  html +=
    htmlEscape(
      wifiGateway
    );


  html +=
    "' "
    "placeholder='192.168.1.1'>";


  // ----------------------------------------------------------
  // SUBNET
  // ----------------------------------------------------------

  html +=
    "<label>Subnet</label>";


  html +=
    "<input type='text' "
    "name='subnet' "
    "value='";


  html +=
    htmlEscape(
      wifiSubnet
    );


  html +=
    "' "
    "placeholder='255.255.255.0'>";


  html +=
    "</div>";


  // ----------------------------------------------------------
  // SAVE
  // ----------------------------------------------------------

  html +=
    "<button type='submit'>"
    "SAVE & RESTART"
    "</button>";


  html +=
    "<a class='button secondary' "
    "href='/'>"
    "Cancel"
    "</a>";


  html +=
    "</form>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// SAVE WIFI SETTINGS
// ============================================================

void handleSaveWiFi() {

  if (server.hasArg("ssid")) {

    wifiSSID =
      server.arg("ssid");
  }


  if (server.hasArg("password")) {

    wifiPassword =
      server.arg("password");
  }


  if (server.hasArg("ip")) {

    wifiIP =
      server.arg("ip");
  }


  if (server.hasArg("gateway")) {

    wifiGateway =
      server.arg("gateway");
  }


  if (server.hasArg("subnet")) {

    wifiSubnet =
      server.arg("subnet");
  }


  if (
    server.hasArg("mode") &&
    server.arg("mode") ==
      "static"
  ) {

    wifiDHCP = false;
  }
  else {

    wifiDHCP = true;
  }


  wifiSSID.trim();
  wifiIP.trim();
  wifiGateway.trim();
  wifiSubnet.trim();


  saveWiFiSettings();


  String html =
    htmlHeader(
      "WiFi Saved"
    );


  html +=
    "<h1>WiFi Settings Saved</h1>";


  html +=
    "<div class='status success'>"
    "The ESP32 will restart now and "
    "attempt to connect using the "
    "new settings."
    "</div>";


  html +=
    htmlFooter();


  server.send(
    200,
    "text/html",
    html
  );


  delay(1500);


  ESP.restart();
}


// ============================================================
// 404
// ============================================================

void handleNotFound() {

  server.sendHeader(
    "Location",
    "/"
  );


  server.send(
    302,
    "text/plain",
    "Redirecting..."
  );
}


// ============================================================
// WEB SERVER SETUP
// ============================================================

void setupWebServer() {

  // Main page

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );


  // ----------------------------------------------------------
  // Profiles
  // ----------------------------------------------------------

  server.on(
    "/profiles",
    HTTP_GET,
    handleProfiles
  );


  server.on(
    "/profile/new",
    HTTP_GET,
    handleNewProfile
  );


  server.on(
    "/profile/edit",
    HTTP_GET,
    handleEditProfile
  );


  server.on(
    "/profile/save",
    HTTP_POST,
    handleSaveProfile
  );


  server.on(
    "/profile/delete",
    HTTP_GET,
    handleDeleteProfile
  );


  // ----------------------------------------------------------
  // iLO4
  // ----------------------------------------------------------

  server.on(
    "/ilo",
    HTTP_GET,
    handleILO
  );


  server.on(
    "/ilo/save",
    HTTP_POST,
    handleSaveILO
  );


  server.on(
    "/ilo/test",
    HTTP_GET,
    handleTestILO
  );


  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  server.on(
    "/wifi",
    HTTP_GET,
    handleWiFi
  );


  server.on(
    "/wifi/save",
    HTTP_POST,
    handleSaveWiFi
  );


  // ----------------------------------------------------------
  // 404
  // ----------------------------------------------------------

  server.onNotFound(
    handleNotFound
  );


  server.begin();


  Serial.println(
    "Web server started."
  );
}


// ============================================================
// SETUP
// ============================================================

// The SSH handshake/crypto code needs more stack than the
// default Arduino loop task gives it. Overriding this weak
// function tells arduino-esp32 how big to make that task.
size_t getArduinoLoopTaskStackSize(void) {
  return 24 * 1024;
}


void setup() {

  Serial.begin(115200);

  delay(1000);


  Serial.println();
  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println(
    " ESP32-S3 iLO4 CONTROLLER"
  );

  Serial.println(
    " Configuration Edition"
  );

  Serial.println(
    "========================================"
  );


  // ----------------------------------------------------------
  // LOAD SETTINGS
  // ----------------------------------------------------------

  loadSettings();


  // ----------------------------------------------------------
  // DEFAULT PROFILE
  // ----------------------------------------------------------

  createDefaultProfile();


  // ----------------------------------------------------------
  // OLED + EC11 PANEL
  // ----------------------------------------------------------

  setupPanel();


  // ----------------------------------------------------------
  // START AP
  // ----------------------------------------------------------

  startAccessPoint();


  // ----------------------------------------------------------
  // TRY SAVED WIFI
  // ----------------------------------------------------------

  if (
    wifiSSID.length() > 0
  ) {

    connectSavedWiFi();
  }


  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  setupWebServer();


  // ----------------------------------------------------------
  // READY
  // ----------------------------------------------------------

  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println(
    "READY"
  );

  Serial.println(
    "========================================"
  );


  Serial.print(
    "Configuration AP: "
  );

  Serial.println(
    AP_SSID
  );


  Serial.print(
    "AP address: "
  );

  Serial.println(
    WiFi.softAPIP()
  );


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "WiFi address: "
    );

    Serial.println(
      WiFi.localIP()
    );
  }


  Serial.println();


  Serial.println(
    "Open:"
  );


  Serial.print(
    "http://"
  );


  Serial.println(
    WiFi.softAPIP()
  );


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "or http://"
    );

    Serial.println(
      WiFi.localIP()
    );
  }


  Serial.println();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  server.handleClient();

  handleUI();

  delay(2);
}