#include <SPI.h>
#include <MFRC522.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "time.h"

// =========================================================
//                  NETWORK & TIME CONFIGURATION
// =========================================================
const char* ssid     = "Redmi 9 Power"; // Hotspot SSID
const char* password = "shanmugam";     // Hotspot Password

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;      // IST (+5:30)
const int   daylightOffset_sec = 0;

WebServer server(80);
Preferences prefs;
Preferences logPrefs;

// =========================================================
//                  HARDWARE PINOUTS & DATA STRUCTURES
// =========================================================
#define SS_PIN          5
#define RST_PIN         4
#define STATUS_LED      2
#define DF_RX_PIN      26
#define DF_TX_PIN      27
#define VELOCITY_LOCK_MS 2000

enum AttendanceState { STATE_ABSENT, STATE_PRESENT, STATE_LATE };

struct Student {
  String uid;
  String rollNo;
  String name;
  int trackNo; // Folder 04 track number
  bool isFaculty; // Flag to identify faculty
  AttendanceState sessionState[7];
  int anomalyScore; 
};

#define MAX_STUDENTS 30
Student studentDB[MAX_STUDENTS];
int totalStudents = 0;

// Activity Log Structure (For 1-Month / Historical Report)
#define MAX_LOGS 100
struct ActivityRecord {
  char timestamp[20];
  char personName[30];
  char role[10];
  char status[40];
};

ActivityRecord activityLogs[MAX_LOGS];
int logCount = 0;

struct PeriodWindow {
  int startHour, startMin;
  int endHour, endMin;
  int warningHour, warningMin;
};

// 7 Period Schedule
PeriodWindow periods[7] = {
  { 9,  0,  9, 55,  8, 55}, // P1
  { 9, 55, 10, 50,  9, 50}, // P2
  {11, 10, 12,  0, 11,  5}, // P3
  {12,  0, 12, 50, 11, 55}, // P4
  {13, 40, 14, 30, 13, 35}, // P5
  {14, 30, 15, 20, 14, 25}, // P6
  {15, 20, 16, 10, 15, 15}  // P7
};

// Timetable mapping: Days 0-4 (Mon-Fri) x Periods 0-6 (Track numbers in Folder 02)
int weeklyTimetable[5][7] = {
  { 1,  2,  3,  4,  9,  9, 10},
  { 2,  3,  1,  5,  7,  7,  7},
  { 4,  5,  2,  1,  8,  8,  8},
  { 3,  1,  4,  2,  9,  9, 10},
  { 5,  4,  1,  3, 12, 13, 11}
};

int saturdayFollowDay = 0;

MFRC522 rfid(SS_PIN, RST_PIN);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini mp3;

unsigned long lastGlobalScanTime = 0;
String lastActivityLog = "System Initialized";
int lastWarningP = -1, lastStartP = -1;

// Forward Declarations
bool isAuthenticated(bool requireFaculty = false);
void playAudio(int folder, int track, int delayMs);
void addActivityLog(String name, String role, String status);

// =========================================================
//                  PERSISTENCE (NVS)
// =========================================================
void loadDefaults() {
  totalStudents = 3;
  // Shanmugam T set as Faculty (isFaculty = true)
  studentDB[0] = {"04DE0B06", "FAC001", "Shanmugam T", 1, true, {STATE_ABSENT}, 0};
  studentDB[1] = {"1079F25C", "22BEE016", "Mohamath Ashik N", 2, false, {STATE_ABSENT}, 0};
  studentDB[2] = {"DCBB4906", "22BCE102", "Abinesh R", 3, false, {STATE_ABSENT}, 0};
}

void saveStudentsToFlash() {
  prefs.begin("db", false);
  prefs.putInt("count", totalStudents);
  for (int i = 0; i < totalStudents; i++) {
    String prefix = "s" + String(i);
    prefs.putString((prefix + "u").c_str(), studentDB[i].uid);
    prefs.putString((prefix + "r").c_str(), studentDB[i].rollNo);
    prefs.putString((prefix + "n").c_str(), studentDB[i].name);
    prefs.putInt((prefix + "t").c_str(), studentDB[i].trackNo);
    prefs.putBool((prefix + "f").c_str(), studentDB[i].isFaculty);
  }
  prefs.end();
}

void loadStudentsFromFlash() {
  prefs.begin("db", true);
  totalStudents = prefs.getInt("count", -1);
  if (totalStudents == -1) {
    prefs.end();
    loadDefaults();
    saveStudentsToFlash();
    return;
  }
  for (int i = 0; i < totalStudents; i++) {
    String prefix = "s" + String(i);
    studentDB[i].uid = prefs.getString((prefix + "u").c_str(), "");
    studentDB[i].rollNo = prefs.getString((prefix + "r").c_str(), "");
    studentDB[i].name = prefs.getString((prefix + "n").c_str(), "");
    studentDB[i].trackNo = prefs.getInt((prefix + "t").c_str(), 1);
    studentDB[i].isFaculty = prefs.getBool((prefix + "f").c_str(), false);
    studentDB[i].anomalyScore = 0;
    for (int s = 0; s < 7; s++) studentDB[i].sessionState[s] = STATE_ABSENT;
  }
  prefs.end();
}

void saveTimetableToFlash() {
  prefs.begin("ttable", false);
  prefs.putInt("sat_day", saturdayFollowDay);
  for (int d = 0; d < 5; d++) {
    for (int p = 0; p < 7; p++) {
      String key = "t_" + String(d) + "_" + String(p);
      prefs.putInt(key.c_str(), weeklyTimetable[d][p]);
    }
  }
  prefs.end();
}

void loadTimetableFromFlash() {
  prefs.begin("ttable", true);
  saturdayFollowDay = prefs.getInt("sat_day", 0);
  if (prefs.isKey("t_0_0")) {
    for (int d = 0; d < 5; d++) {
      for (int p = 0; p < 7; p++) {
        String key = "t_" + String(d) + "_" + String(p);
        weeklyTimetable[d][p] = prefs.getInt(key.c_str(), weeklyTimetable[d][p]);
      }
    }
  }
  prefs.end();
}

void addActivityLog(String name, String role, String status) {
  struct tm timeinfo;
  char timeBuf[20] = "N/A";
  if (getLocalTime(&timeinfo, 10)) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d/%02d %02d:%02d:%02d", 
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  if (logCount < MAX_LOGS) {
    strncpy(activityLogs[logCount].timestamp, timeBuf, 19);
    strncpy(activityLogs[logCount].personName, name.c_str(), 29);
    strncpy(activityLogs[logCount].role, role.c_str(), 9);
    strncpy(activityLogs[logCount].status, status.c_str(), 39);
    logCount++;
  } else {
    // Shift logs to maintain rolling window
    for (int i = 0; i < MAX_LOGS - 1; i++) {
      activityLogs[i] = activityLogs[i + 1];
    }
    strncpy(activityLogs[MAX_LOGS - 1].timestamp, timeBuf, 19);
    strncpy(activityLogs[MAX_LOGS - 1].personName, name.c_str(), 29);
    strncpy(activityLogs[MAX_LOGS - 1].role, role.c_str(), 9);
    strncpy(activityLogs[MAX_LOGS - 1].status, status.c_str(), 39);
  }
}

// =========================================================
//                  AUDIO ANNOUNCEMENT LOGIC
// =========================================================
void playAudio(int folder, int track, int delayMs) {
  mp3.playFolder(folder, track);
  delay(delayMs);
}

void announcePrePeriod(int periodIdx, int dayIdx) {
  int effectiveDay = (dayIdx == 5) ? saturdayFollowDay : dayIdx;
  int subjectTrack = weeklyTimetable[effectiveDay][periodIdx];
  playAudio(1, periodIdx + 1, 1500); 
  playAudio(1, 8, 2500);             
  playAudio(2, subjectTrack, 2000);   
}

void announcePeriodStart(int periodIdx) {
  playAudio(1, periodIdx + 1, 1500); 
  playAudio(1, 12, 1800);             
}

// =========================================================
//              ENHANCED RFID & ATTENDANCE WINDOW
// =========================================================
void processRFIDScan(String uid) {
  unsigned long now = millis();
  if (now - lastGlobalScanTime < VELOCITY_LOCK_MS) return;
  lastGlobalScanTime = now;

  int foundIdx = -1;
  for (int i = 0; i < totalStudents; i++) {
    if (studentDB[i].uid.equalsIgnoreCase(uid)) {
      foundIdx = i;
      break;
    }
  }

  if (foundIdx == -1) {
    lastActivityLog = "Unknown Card: " + uid;
    Serial.println("❌ " + lastActivityLog);
    playAudio(3, 2, 2000);
    addActivityLog("Unknown", "N/A", "Access Denied");
    return;
  }

  struct tm timeinfo;
  int validAttendancePeriod = -1;
  char currentTimeStr[10] = "";

  if (getLocalTime(&timeinfo, 10)) {
    snprintf(currentTimeStr, sizeof(currentTimeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    int curMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    // Strict Window Checking: [-5 mins, +2 mins] around period start
    for (int p = 0; p < 7; p++) {
      int pStartMin = periods[p].startHour * 60 + periods[p].startMin;
      int windowOpen = pStartMin - 5;
      int windowClose = pStartMin + 2;

      if (curMin >= windowOpen && curMin <= windowClose) {
        validAttendancePeriod = p;
        break;
      }
    }
  }

  String roleStr = studentDB[foundIdx].isFaculty ? "Faculty" : "Student";

  // CASE 1: Inside Attendance Window (-5 mins to +2 mins)
  if (validAttendancePeriod != -1) {
    if (studentDB[foundIdx].sessionState[validAttendancePeriod] == STATE_PRESENT) {
      lastActivityLog = "Already Recorded: " + studentDB[foundIdx].name;
      playAudio(3, 3, 2200); 
      playAudio(4, studentDB[foundIdx].trackNo, 1800); 
      addActivityLog(studentDB[foundIdx].name, roleStr, "Already Present (P" + String(validAttendancePeriod + 1) + ")");
    } else {
      studentDB[foundIdx].sessionState[validAttendancePeriod] = STATE_PRESENT;
      lastActivityLog = "Marked PRESENT: " + studentDB[foundIdx].name + " (P" + String(validAttendancePeriod + 1) + ")";
      playAudio(3, 1, 2000); 
      playAudio(4, studentDB[foundIdx].trackNo, 1800); 
      addActivityLog(studentDB[foundIdx].name, roleStr, "Marked PRESENT (P" + String(validAttendancePeriod + 1) + ")");
    }
  } 
  // CASE 2: Outside Attendance Window (Announce Name & Record Entry Time)
  else {
    lastActivityLog = "Logged Entry: " + studentDB[foundIdx].name + " at " + String(currentTimeStr);
    Serial.println("ℹ️ " + lastActivityLog);
    
    // Play Name Audio (Folder 04, Track No)
    playAudio(4, studentDB[foundIdx].trackNo, 1800); 
    addActivityLog(studentDB[foundIdx].name, roleStr, "Entry Logged @ " + String(currentTimeStr));
  }
}

// =========================================================
//            AUTHENTICATION & ENHANCED API HANDLERS
// =========================================================
bool isAuthenticated(bool requireFaculty) {
  if (!server.hasHeader("Cookie")) return false;
  String cookie = server.header("Cookie");
  if (requireFaculty) {
    return cookie.indexOf("AUTH_ROLE=FACULTY") != -1;
  }
  return (cookie.indexOf("AUTH_ROLE=FACULTY") != -1 || cookie.indexOf("AUTH_ROLE=STUDENT") != -1);
}

void handleLogin() {
  if (server.method() == HTTP_POST) {
    String role = server.arg("role");
    String pass = server.arg("password");

    if (role == "faculty" && pass == "admin123") {
      server.sendHeader("Set-Cookie", "AUTH_ROLE=FACULTY; Path=/");
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    } else if (role == "student" && pass == "student123") {
      server.sendHeader("Set-Cookie", "AUTH_ROLE=STUDENT; Path=/");
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }
  }

  String html = R"rawliteral(
  <!DOCTYPE html><html><head><title>Smart Campus Portal</title>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <style>
    body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;background:linear-gradient(135deg, #1e3c72 0%, #2a5298 100%);height:100vh;display:flex;align-items:center;justify-content:center;}
    .card{background:#fff;padding:30px;border-radius:15px;box-shadow:0 10px 25px rgba(0,0,0,0.3);width:90%;max-width:380px;}
    h2{margin-top:0;color:#1e3c72;text-align:center;font-size:24px;}
    label{font-weight:600;color:#555;font-size:14px;margin-top:10px;display:block;}
    input,select{width:100%;padding:12px;margin:8px 0 15px;box-sizing:border-box;border:1px solid #ccc;border-radius:8px;font-size:15px;}
    button{width:100%;padding:12px;background:linear-gradient(90deg, #007bff, #0056b3);color:#fff;border:none;border-radius:8px;font-weight:bold;font-size:16px;cursor:pointer;transition:0.3s;}
    button:hover{opacity:0.9;}
  </style></head><body>
  <div class='card'>
    <h2>🏫 Smart Attendance</h2>
    <form method='POST' action='/login'>
      <label>Select Role</label>
      <select name='role'><option value='student'>Student</option><option value='faculty'>Faculty</option></select>
      <label>Password</label>
      <input type='password' name='password' placeholder='Enter Password' required>
      <button type='submit'>Sign In</button>
    </form>
  </div></body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleLogout() {
  server.sendHeader("Set-Cookie", "AUTH_ROLE=LOGGED_OUT; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
  server.sendHeader("Location", "/login");
  server.send(303);
}

String getStateName(AttendanceState state) {
  switch (state) {
    case STATE_PRESENT: return "<span class='badge bg-success'>PRESENT</span>";
    case STATE_LATE: return "<span class='badge bg-warning'>LATE</span>";
    default: return "<span class='badge bg-danger'>ABSENT</span>";
  }
}

// Fully Modernized Mobile App Interface
void handleRoot() {
  if (!isAuthenticated(false)) {
    server.sendHeader("Location", "/login");
    server.send(303);
    return;
  }

  bool isFaculty = isAuthenticated(true);

  String html = R"rawliteral(
  <!DOCTYPE html><html><head><title>Smart Campus App</title>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <style>
    body{font-family:'Segoe UI',Arial,sans-serif;margin:0;background:#f0f2f5;color:#333;}
    .header{background:linear-gradient(90deg, #1e3c72, #2a5298);color:#fff;padding:20px;text-align:center;border-bottom-left-radius:20px;border-bottom-right-radius:20px;box-shadow:0 4px 10px rgba(0,0,0,0.15);}
    .header h2{margin:0;font-size:22px;}
    .container{padding:15px;max-width:600px;margin:auto;}
    .card{background:#fff;padding:18px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,0.08);margin-bottom:15px;}
    .card h3{margin-top:0;font-size:16px;color:#1e3c72;border-bottom:2px solid #f0f2f5;padding-bottom:8px;}
    .status-box{background:#eef2f7;padding:12px;border-radius:8px;font-size:13px;margin-bottom:10px;}
    .btn{display:block;width:100%;padding:12px;margin:8px 0;background:#007bff;color:#fff;text-align:center;border-radius:8px;text-decoration:none;font-weight:bold;border:none;box-sizing:border-box;font-size:14px;cursor:pointer;}
    .btn-success{background:#28a745;} .btn-warning{background:#ffc107;color:#000;} .btn-danger{background:#dc3545;} .btn-info{background:#17a2b8;} .btn-dark{background:#343a40;}
    table{width:100%;border-collapse:collapse;font-size:12px;margin-top:10px;}
    th,td{padding:8px 4px;border-bottom:1px solid #eee;text-align:center;}
    th{background:#1e3c72;color:#fff;}
    .badge{padding:4px 7px;border-radius:4px;color:#fff;font-weight:bold;font-size:10px;}
    .bg-success{background:#28a745;} .bg-danger{background:#dc3545;} .bg-warning{background:#ffc107;color:#000;}
    select{width:100%;padding:10px;margin:5px 0;border-radius:6px;border:1px solid #ccc;}
    .nav-tabs{display:flex;gap:5px;margin-bottom:15px;}
    .tab-btn{flex:1;padding:10px;background:#ddd;text-align:center;border-radius:8px;cursor:pointer;font-weight:bold;font-size:13px;}
    .tab-btn.active{background:#1e3c72;color:#fff;}
  </style></head><body>
  <div class='header'>
    <h2>🎓 Smart Campus Dashboard</h2>
  </div>
  <div class='container'>
    <div class='nav-tabs'>
      <div class='tab-btn active' onclick='showTab("live")'>📱 Live Matrix</div>
      <div class='tab-btn' onclick='showTab("report")'>📊 1-Month Report</div>
    </div>

    <div id='tab-live'>
      <div class='card'>
        <h3>📌 Real-Time Overview</h3>
        <div class='status-box'>
          <p><strong>Logged Role:</strong> )rawliteral" + String(isFaculty ? "Faculty Member" : "Student") + R"rawliteral()</p>
          <p><strong>System Clock:</strong> <span id='clock'>Syncing...</span></p>
          <p><strong>Latest Activity:</strong> <span id='activity'>)rawliteral" + lastActivityLog + R"rawliteral(</span></p>
        </div>
        <a href='/api/download-attendance' class='btn btn-info'>📥 Download Attendance (CSV)</a>
        )rawliteral";

  if (isFaculty) {
    html += R"rawliteral(
        <a href='/admin' class='btn btn-dark'>⚙️ Faculty Admin & Timetable</a>
        <button class='btn btn-warning' onclick="fetch('/api/random-call').then(r=>r.json()).then(d=>alert('Audit Triggered! Called ' + d.calledCount + ' users.'));">🎯 Run Malpractice Audit</button>
        <hr>
        <label><strong>🔔 Manual Period Bell Announcement:</strong></label>
        <select id='bellSelect'>
    )rawliteral";
    for (int p = 0; p < 7; p++) {
      html += "<option value='" + String(p + 1) + "'>Period " + String(p + 1) + " Bell</option>";
    }
    html += R"rawliteral(
        </select>
        <button class='btn btn-success' onclick="fetch('/api/trigger-bell?p=' + document.getElementById('bellSelect').value)">🔔 Ring Period Bell</button>
    )rawliteral";
  }

  html += R"rawliteral(
        <a href='/logout' class='btn btn-danger'>🚪 Logout</a>
      </div>

      <div class='card'>
        <h3>📋 Live Attendance Board</h3>
        <div id='matrix'>Loading...</div>
      </div>
    </div>

    <div id='tab-report' style='display:none;'>
      <div class='card'>
        <h3>📊 Monthly / Historical Activity Report</h3>
        <p style='font-size:12px;color:#666;'>Logs of attendance scans, period entries, and timestamps.</p>
        <a href='/api/download-activity-report' class='btn btn-info'>📥 Download Full Month Report (CSV)</a>
        <div id='activityReport'>Loading Logs...</div>
      </div>
    </div>
  </div>

  <script>
    function showTab(tab) {
      document.getElementById('tab-live').style.display = (tab === 'live') ? 'block' : 'none';
      document.getElementById('tab-report').style.display = (tab === 'report') ? 'block' : 'none';
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      if(tab === 'live') document.querySelectorAll('.tab-btn')[0].classList.add('active');
      else document.querySelectorAll('.tab-btn')[1].classList.add('active');
    }

    function refreshData() {
      fetch('/api/status').then(res=>res.json()).then(d=>{
        document.getElementById('clock').innerText = d.time;
        document.getElementById('activity').innerText = d.log;
      });

      fetch('/api/students').then(res=>res.json()).then(data=>{
        let h = '<table><tr><th>Roll/ID</th><th>Name</th><th>P1</th><th>P2</th><th>P3</th><th>P4</th><th>P5</th><th>P6</th><th>P7</th></tr>';
        data.students.forEach(s=>{
          h += `<tr><td><b>${s.rollNo}</b><br><small>(${s.isFaculty ? 'Faculty' : 'Student'})</small></td><td>${s.name}</td>`;
          s.sessions.forEach(st=>{ h += `<td>${st}</td>`; });
          h += '</tr>';
        });
        h += '</table>';
        document.getElementById('matrix').innerHTML = h;
      });

      fetch('/api/activity-logs').then(res=>res.json()).then(data=>{
        let h = '<table><tr><th>Time</th><th>Name</th><th>Role</th><th>Status</th></tr>';
        data.logs.forEach(l=>{
          h += `<tr><td>${l.time}</td><td>${l.name}</td><td>${l.role}</td><td>${l.status}</td></tr>`;
        });
        h += '</table>';
        document.getElementById('activityReport').innerHTML = h;
      });
    }

    setInterval(refreshData, 3000);
    refreshData();
  </script>
  </body></html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleAdmin() {
  if (!isAuthenticated(true)) {
    server.sendHeader("Location", "/login");
    server.send(303);
    return;
  }

  String html = "<!DOCTYPE html><html><head><title>Faculty Control Panel</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:15px;background:#e9ecef;} ";
  html += ".card{background:#fff;padding:15px;border-radius:8px;margin-bottom:15px;} ";
  html += "input,select{width:100%;padding:8px;margin:5px 0 10px;box-sizing:border-box;} ";
  html += "button{padding:10px;background:#007bff;color:#fff;border:none;border-radius:5px;width:100%;font-weight:bold;} ";
  html += "table{width:100%;border-collapse:collapse;font-size:11px;} th,td{padding:4px;border:1px solid #ddd;text-align:center;} th{background:#343a40;color:#fff;} ";
  html += ".del{background:#dc3545;color:#fff;padding:4px 8px;text-decoration:none;border-radius:3px;}";
  html += "</style></head><body>";
  html += "<h2>👨‍🏫 Faculty Control Center</h2><a href='/'>← Back to Main Dashboard</a><br><br>";

  html += "<div class='card'><h3> Add / Edit Student or Faculty</h3>";
  html += "<form action='/admin/save-student' method='POST'>";
  html += "<input type='hidden' name='idx' value='-1'>";
  html += "<label>RFID UID:</label><input type='text' name='uid' placeholder='e.g. 04DE0B06' required>";
  html += "<label>Roll No / Faculty ID:</label><input type='text' name='roll' placeholder='e.g. 22BEE015 or FAC001' required>";
  html += "<label>Full Name:</label><input type='text' name='name' placeholder='Full Name' required>";
  html += "<label>Folder 04 Track No (Audio Name Track):</label><input type='number' name='track' value='1' required>";
  html += "<label>User Type:</label><select name='isFaculty'><option value='0'>Student</option><option value='1'>Faculty</option></select>";
  html += "<button type='submit'>➕ Save User Profile</button></form></div>";

  html += "<div class='card'><h3>Registered Profiles (" + String(totalStudents) + ")</h3><table>";
  html += "<tr><th>UID</th><th>Roll/ID</th><th>Name</th><th>Role</th><th>Track</th><th>Actions</th></tr>";
  for (int i = 0; i < totalStudents; i++) {
    html += "<tr><td>" + studentDB[i].uid + "</td><td>" + studentDB[i].rollNo + "</td><td>" + studentDB[i].name + "</td><td>" + String(studentDB[i].isFaculty ? "Faculty" : "Student") + "</td><td>" + String(studentDB[i].trackNo) + "</td>";
    html += "<td><a class='del' href='/admin/delete-student?idx=" + String(i) + "'>Delete</a></td></tr>";
  }
  html += "</table></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveStudent() {
  if (!isAuthenticated(true)) return;
  if (server.hasArg("uid") && server.hasArg("name")) {
    int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
    if (idx == -1 && totalStudents < MAX_STUDENTS) {
      idx = totalStudents++;
    }
    if (idx >= 0 && idx < MAX_STUDENTS) {
      studentDB[idx].uid = server.arg("uid");
      studentDB[idx].uid.toUpperCase();
      studentDB[idx].rollNo = server.arg("roll");
      studentDB[idx].name = server.arg("name");
      studentDB[idx].trackNo = server.arg("track").toInt();
      studentDB[idx].isFaculty = (server.arg("isFaculty") == "1");
      if (idx == totalStudents - 1) {
        for (int s = 0; s < 7; s++) studentDB[idx].sessionState[s] = STATE_ABSENT;
      }
      saveStudentsToFlash();
    }
  }
  server.sendHeader("Location", "/admin");
  server.send(303);
}

void handleDeleteStudent() {
  if (!isAuthenticated(true)) return;
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < totalStudents) {
      for (int i = idx; i < totalStudents - 1; i++) {
        studentDB[i] = studentDB[i + 1];
      }
      totalStudents--;
      saveStudentsToFlash();
    }
  }
  server.sendHeader("Location", "/admin");
  server.send(303);
}

void handleDownloadAttendance() {
  if (!isAuthenticated(false)) return;
  String csv = "Roll Number,Name,Role,P1,P2,P3,P4,P5,P6,P7\n";
  for (int i = 0; i < totalStudents; i++) {
    csv += studentDB[i].rollNo + "," + studentDB[i].name + "," + (studentDB[i].isFaculty ? "Faculty" : "Student");
    for (int p = 0; p < 7; p++) {
      csv += ",";
      csv += (studentDB[i].sessionState[p] == STATE_PRESENT ? "PRESENT" : "ABSENT");
    }
    csv += "\n";
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=attendance_report.csv");
  server.send(200, "text/csv", csv);
}

void handleDownloadActivityReport() {
  if (!isAuthenticated(false)) return;
  String csv = "Timestamp,Name,Role,Status/Action\n";
  for (int i = 0; i < logCount; i++) {
    csv += String(activityLogs[i].timestamp) + "," + String(activityLogs[i].personName) + "," + String(activityLogs[i].role) + "," + String(activityLogs[i].status) + "\n";
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=monthly_activity_report.csv");
  server.send(200, "text/csv", csv);
}

void handleAPIStatus() {
  struct tm timeinfo;
  char timeBuf[45];
  if (getLocalTime(&timeinfo, 10)) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "Syncing...");
  }
  String json = "{\"time\":\"" + String(timeBuf) + "\",\"log\":\"" + lastActivityLog + "\"}";
  server.send(200, "application/json", json);
}

void handleAPIStudents() {
  String json = "{\"students\":[";
  for (int i = 0; i < totalStudents; i++) {
    json += "{\"rollNo\":\"" + studentDB[i].rollNo + "\",\"name\":\"" + studentDB[i].name + "\",\"isFaculty\":" + String(studentDB[i].isFaculty ? "true" : "false") + ",\"sessions\":[";
    for (int s = 0; s < 7; s++) {
      json += "\"" + getStateName(studentDB[i].sessionState[s]) + "\"" + (s < 6 ? "," : "");
    }
    json += String("]}") + (i < totalStudents - 1 ? "," : "");
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleAPIActivityLogs() {
  String json = "{\"logs\":[";
  for (int i = logCount - 1; i >= 0; i--) {
    json += "{\"time\":\"" + String(activityLogs[i].timestamp) + "\",\"name\":\"" + String(activityLogs[i].personName) + "\",\"role\":\"" + String(activityLogs[i].role) + "\",\"status\":\"" + String(activityLogs[i].status) + "\"}" + (i > 0 ? "," : "");
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleAPITriggerBell() {
  if (!isAuthenticated(true)) return;
  int periodVal = server.hasArg("p") ? server.arg("p").toInt() : 1;
  lastActivityLog = "Manual Bell Triggered for Period " + String(periodVal);
  playAudio(1, periodVal, 1500); 
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// =========================================================
//                  SETUP & MAIN LOOP
// =========================================================
void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  mp3.begin(dfSerial);
  mp3.volume(25);

  loadStudentsFromFlash();
  loadTimetableFromFlash();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Mobile Hotspot");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi Connected!");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  const char * headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);
  server.on("/admin", handleAdmin);
  server.on("/admin/save-student", HTTP_POST, handleSaveStudent);
  server.on("/admin/delete-student", handleDeleteStudent);
  server.on("/api/download-attendance", handleDownloadAttendance);
  server.on("/api/download-activity-report", handleDownloadActivityReport);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/students", handleAPIStudents);
  server.on("/api/activity-logs", handleAPIActivityLogs);
  server.on("/api/trigger-bell", handleAPITriggerBell);
  server.begin();

  Serial.print("🌐 Mobile Dashboard URL: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    int dayIdx = timeinfo.tm_wday - 1;
    if (dayIdx >= 0 && dayIdx <= 5) {
      for (int p = 0; p < 7; p++) {
        if (timeinfo.tm_hour == periods[p].warningHour && timeinfo.tm_min == periods[p].warningMin && lastWarningP != p) {
          lastWarningP = p;
          lastActivityLog = "Warning: P" + String(p + 1) + " starting in 5 mins";
          announcePrePeriod(p, dayIdx);
        }
        if (timeinfo.tm_hour == periods[p].startHour && timeinfo.tm_min == periods[p].startMin && lastStartP != p) {
          lastStartP = p;
          lastActivityLog = "Period " + String(p + 1) + " Started";
          announcePeriodStart(p);
        }
      }
    }
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String scannedUID = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) scannedUID += "0";
      scannedUID += String(rfid.uid.uidByte[i], HEX);
    }
    scannedUID.toUpperCase();

    processRFIDScan(scannedUID);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}