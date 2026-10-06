#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

static const char *AP_SSID = "KneePad_ESP32";
static const char *AP_PASSWORD = "12345678";
static const char *DEVICE_PAIR_CODE = "2580";

// ESP32-S3 reads STM32 lines like:
// KNEE,score,action,count,alert,quality,label,fail_mask,contact
static const int STM32_RX_PIN = 18;
static const int STM32_TX_PIN = 17;
static const uint32_t STM32_BAUD = 115200;
static const uint8_t MAX_PROFILES = 6;

WebServer server(80);
HardwareSerial Stm32Serial(1);
Preferences preferences;

struct UserProfile
{
  String name;
  String gender;
  uint16_t age;
  uint16_t heightCm;
  uint16_t weightX10;
  String exerciseHabit;
  String goal;
  String injury;
  uint32_t totalReps;
};

String latestLine = "KNEE,0,UNKNOWN,0,0,0,WAIT,0,WAIT";
String latestAction = "UNKNOWN";
uint32_t latestScore = 0;
uint32_t latestCount = 0;
uint32_t latestAlert = 0;
uint32_t latestQuality = 0;
uint32_t latestQualityFailMask = 0;
String latestQualityLabel = "WAIT";
String latestContact = "WAIT";
uint32_t latestWalkCount = 0;
uint32_t latestSquatCount = 0;
uint32_t latestDeadliftCount = 0;
uint32_t latestUpdateMs = 0;
String rxLine;
String debugRxLine;

// STM32 dual-IMU posture stream:
// PRESS,med=...,lat=...,diff=...,status=BAL/MED/LAT/CAL/ERR/OFF,...
uint32_t latestPressMed = 0;
uint32_t latestPressLat = 0;
int32_t latestPressDiff = 0;
String latestPressStatus = "OFF";
uint32_t latestPressMs = 0;
uint32_t telemetryStreamAskedMs = 0;

int8_t activeProfileIndex = -1;
UserProfile activeProfile;
uint32_t sessionReps = 0;
uint32_t sessionLastDeviceCount = 0;
bool trainingActive = false;
uint32_t trainingStartedMs = 0;
uint32_t trainingStartDeviceCount = 0;
bool profileDirty = false;
uint32_t profileDirtySinceMs = 0;

static String jsonEscape(const String &s)
{
  String out;
  out.reserve(s.length() + 12);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c != '\r') {
      out += c;
    }
  }
  return out;
}

static String profileKey(uint8_t index, const char *suffix)
{
  return "p" + String(index) + suffix;
}

static uint8_t profileCount()
{
  uint8_t count = preferences.getUChar("pcount", 0);
  return count > MAX_PROFILES ? MAX_PROFILES : count;
}

static bool loadProfile(uint8_t index, UserProfile &profile)
{
  if (index >= profileCount()) {
    return false;
  }

  profile.name = preferences.getString(profileKey(index, "n").c_str(), "");
  profile.gender = preferences.getString(profileKey(index, "g").c_str(), "未设置");
  profile.age = preferences.getUShort(profileKey(index, "a").c_str(), 0);
  profile.heightCm = preferences.getUShort(profileKey(index, "h").c_str(), 0);
  profile.weightX10 = preferences.getUShort(profileKey(index, "w").c_str(), 0);
  profile.exerciseHabit = preferences.getString(profileKey(index, "e").c_str(), "偶尔运动");
  profile.goal = preferences.getString(profileKey(index, "o").c_str(), "日常锻炼");
  profile.injury = preferences.getString(profileKey(index, "i").c_str(), "无");
  profile.totalReps = preferences.getUInt(profileKey(index, "r").c_str(), 0);
  return profile.name.length() > 0;
}

static void saveProfile(uint8_t index, const UserProfile &profile)
{
  preferences.putString(profileKey(index, "n").c_str(), profile.name);
  preferences.putString(profileKey(index, "g").c_str(), profile.gender);
  preferences.putUShort(profileKey(index, "a").c_str(), profile.age);
  preferences.putUShort(profileKey(index, "h").c_str(), profile.heightCm);
  preferences.putUShort(profileKey(index, "w").c_str(), profile.weightX10);
  preferences.putString(profileKey(index, "e").c_str(), profile.exerciseHabit);
  preferences.putString(profileKey(index, "o").c_str(), profile.goal);
  preferences.putString(profileKey(index, "i").c_str(), profile.injury);
  preferences.putUInt(profileKey(index, "r").c_str(), profile.totalReps);
}

static void clearProfile(uint8_t index)
{
  preferences.remove(profileKey(index, "n").c_str());
  preferences.remove(profileKey(index, "g").c_str());
  preferences.remove(profileKey(index, "a").c_str());
  preferences.remove(profileKey(index, "h").c_str());
  preferences.remove(profileKey(index, "w").c_str());
  preferences.remove(profileKey(index, "e").c_str());
  preferences.remove(profileKey(index, "o").c_str());
  preferences.remove(profileKey(index, "i").c_str());
  preferences.remove(profileKey(index, "r").c_str());
}

static bool pairCodeValid()
{
  return server.hasArg("pair_code") && server.arg("pair_code") == DEVICE_PAIR_CODE;
}

static void flushActiveProfile()
{
  if (activeProfileIndex < 0 || !profileDirty) {
    return;
  }
  preferences.putUInt(profileKey((uint8_t)activeProfileIndex, "r").c_str(), activeProfile.totalReps);
  profileDirty = false;
}

static uint16_t personalizedFatigueThreshold(const UserProfile &profile)
{
  uint16_t threshold = 70;
  if (profile.age >= 60) threshold = 60;
  else if (profile.age >= 50) threshold = 65;

  if (profile.exerciseHabit == "很少运动" && threshold > 66) threshold = 66;
  if (profile.exerciseHabit == "偶尔运动" && threshold > 72) threshold = 72;
  if (profile.exerciseHabit == "经常运动" && profile.age < 50) threshold = 78;
  if (profile.injury.length() > 0 && profile.injury != "无" && threshold > 70) threshold = 70;
  return threshold;
}

static uint16_t personalizedTargetReps(const UserProfile &profile)
{
  uint16_t target = 10;
  if (profile.exerciseHabit == "很少运动") target = 8;
  else if (profile.exerciseHabit == "偶尔运动") target = 10;
  else if (profile.exerciseHabit == "规律运动") target = 12;
  else if (profile.exerciseHabit == "经常运动") target = 15;
  if (profile.age >= 60 || (profile.injury.length() > 0 && profile.injury != "无")) {
    if (target > 8) target = 8;
  }
  return target;
}

static float profileBmi(const UserProfile &profile)
{
  if (profile.heightCm == 0 || profile.weightX10 == 0) return 0.0f;
  float heightM = profile.heightCm / 100.0f;
  float weightKg = profile.weightX10 / 10.0f;
  return weightKg / (heightM * heightM);
}

static String personalizedAdvice()
{
  if (activeProfileIndex < 0) return "请先选择或创建个人档案";
  uint16_t threshold = personalizedFatigueThreshold(activeProfile);
  uint16_t target = personalizedTargetReps(activeProfile);
  if (latestUpdateMs == 0 || millis() - latestUpdateMs > 2500) return "正在等待护膝检测数据";
  if (latestContact == "LOW" || latestContact == "SAT" || latestContact == "NOISY") return "请检查肌电电极贴合状态";
  if (latestScore >= threshold || latestAlert != 0) return "疲劳值已达到个人提醒线，建议暂停并充分休息";
  if (latestQualityLabel == "WRONG") return "本次动作质量较低，请减慢速度并保持膝关节稳定";
  if (sessionReps >= target) return "已完成本次建议组数，可休息后再继续训练";
  if (activeProfile.injury.length() > 0 && activeProfile.injury != "无") return "检测不能替代医疗判断；如有疼痛请立即停止运动";
  return "保持动作匀速，完成后回到直立位置";
}

static void selectProfile(uint8_t index)
{
  flushActiveProfile();
  UserProfile profile;
  if (!loadProfile(index, profile)) return;
  activeProfileIndex = (int8_t)index;
  activeProfile = profile;
  sessionReps = 0;
  sessionLastDeviceCount = latestCount;
  trainingActive = false;
  trainingStartedMs = 0;
  trainingStartDeviceCount = latestCount;
  profileDirty = false;
}

static void accountCompletedReps(uint32_t deviceCount)
{
  if (activeProfileIndex < 0) return;
  if (deviceCount > sessionLastDeviceCount) {
    uint32_t delta = deviceCount - sessionLastDeviceCount;
    if (delta <= 20) {
      sessionReps += delta;
      activeProfile.totalReps += delta;
      profileDirty = true;
      profileDirtySinceMs = millis();
    }
  }
  sessionLastDeviceCount = deviceCount;
}

static void parseKneeLine(String line)
{
  line.trim();
  if (!line.startsWith("KNEE,")) return;

  int p1 = line.indexOf(',', 5);
  int p2 = (p1 >= 0) ? line.indexOf(',', p1 + 1) : -1;
  int p3 = (p2 >= 0) ? line.indexOf(',', p2 + 1) : -1;
  if (p1 < 0 || p2 < 0 || p3 < 0) return;
  int p4 = line.indexOf(',', p3 + 1);
  int p5 = (p4 >= 0) ? line.indexOf(',', p4 + 1) : -1;
  int p6 = (p5 >= 0) ? line.indexOf(',', p5 + 1) : -1;
  int p7 = (p6 >= 0) ? line.indexOf(',', p6 + 1) : -1;
  int p8 = (p7 >= 0) ? line.indexOf(',', p7 + 1) : -1;
  int p9 = (p8 >= 0) ? line.indexOf(',', p8 + 1) : -1;
  int p10 = (p9 >= 0) ? line.indexOf(',', p9 + 1) : -1;

  latestScore = line.substring(5, p1).toInt();
  latestAction = line.substring(p1 + 1, p2);
  uint32_t newCount = line.substring(p2 + 1, p3).toInt();
  latestAlert = line.substring(p3 + 1, p4 >= 0 ? p4 : line.length()).toInt();
  latestQuality = 0;
  latestQualityFailMask = 0;
  latestQualityLabel = "WAIT";
  latestContact = "WAIT";
  if (p4 >= 0 && p5 >= 0 && p6 >= 0) {
    latestQuality = line.substring(p4 + 1, p5).toInt();
    latestQualityLabel = line.substring(p5 + 1, p6);
    latestQualityFailMask = line.substring(p6 + 1, p7 >= 0 ? p7 : line.length()).toInt();
    if (p7 >= 0) {
      latestContact = line.substring(p7 + 1, p8 >= 0 ? p8 : line.length());
      if (p8 >= 0) latestWalkCount = line.substring(p8 + 1, p9 >= 0 ? p9 : line.length()).toInt();
      if (p9 >= 0) latestSquatCount = line.substring(p9 + 1, p10 >= 0 ? p10 : line.length()).toInt();
      if (p10 >= 0) latestDeadliftCount = line.substring(p10 + 1).toInt();
    }
  }
  accountCompletedReps(newCount);
  latestCount = newCount;
  latestLine = line;
  latestUpdateMs = millis();
}

static long csvValueForKey(const String &line, const char *key, long fallback)
{
  int k = line.indexOf(key);
  if (k < 0) return fallback;
  int eq = line.indexOf('=', k);
  if (eq < 0) return fallback;
  int end = line.indexOf(',', eq + 1);
  if (end < 0) end = line.length();
  return line.substring(eq + 1, end).toInt();
}

static void parsePressLine(String line)
{
  line.trim();
  if (!line.startsWith("PRESS,") || line.indexOf("med=") < 0) return;

  latestPressMed = (uint32_t)csvValueForKey(line, "med", 0);
  latestPressLat = (uint32_t)csvValueForKey(line, "lat", 0);
  latestPressDiff = (int32_t)csvValueForKey(line, "diff", 0);
  int s = line.indexOf("status=");
  if (s >= 0) {
    int end = line.indexOf(',', s + 7);
    if (end < 0) end = line.length();
    latestPressStatus = line.substring(s + 7, end);
  }
  latestPressMs = millis();
}

static void consumeSerial(Stream &port, String &buffer)
{
  while (port.available()) {
    char c = (char)port.read();
    if (c == '\n') {
      if (buffer.startsWith("KNEE,")) parseKneeLine(buffer);
      else if (buffer.startsWith("PRESS,")) parsePressLine(buffer);
      buffer = "";
    } else if (c != '\r') {
      if (buffer.length() < 160) buffer += c;
      else buffer = "";
    }
  }
}

static void handleRoot()
{
  const char *html = R"KNEEHTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>智能护膝运动中心</title>
<style>
:root{--blue:#2867e8;--blue2:#eaf1ff;--ink:#172033;--muted:#6b7690;--line:#dfe5ef;--bg:#f4f7fb;--card:#fff;--red:#dc3c4d;--green:#16885b;--orange:#d37816}*{box-sizing:border-box}body{font-family:"Microsoft YaHei",Arial,sans-serif;margin:0;background:var(--bg);color:var(--ink)}button,input,select,textarea{font:inherit}.wrap{max-width:920px;margin:auto;padding:18px}.top{display:flex;justify-content:space-between;align-items:center;padding:9px 0 17px}.brand{font-size:21px;font-weight:700}.brand small{display:block;font-size:12px;font-weight:400;color:var(--muted);margin-top:3px}.profileBtn,.primary,.secondary{border:0;border-radius:10px;padding:10px 15px;cursor:pointer}.profileBtn,.primary{background:var(--blue);color:#fff}.secondary{background:#eef2f8;color:var(--ink)}.hero{background:var(--blue);color:#fff;border-radius:18px;padding:22px;display:flex;justify-content:space-between;align-items:center;gap:16px}.hero h1{font-size:24px;margin:0 0 8px}.hero p{margin:0;color:#dce7ff}.progress{min-width:140px;text-align:right}.progress strong{font-size:30px}.bar{height:8px;background:#ffffff4a;border-radius:9px;overflow:hidden;margin-top:8px}.bar i{height:100%;display:block;background:#fff;width:0}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:14px 0}.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:17px}.label{color:var(--muted);font-size:13px}.value{font-size:27px;font-weight:700;margin-top:7px}.ok{color:var(--green)}.alert{color:var(--red)}.warn{color:var(--orange)}.row2{display:grid;grid-template-columns:1.3fr 1fr;gap:12px}.detail{color:var(--muted);font-size:14px;margin-top:8px;line-height:1.55}.advice{border-left:4px solid var(--blue)}.userline{display:flex;gap:18px;flex-wrap:wrap;margin-top:14px;color:var(--muted);font-size:13px}.raw{font-family:monospace;word-break:break-all;font-size:12px}.modal{position:fixed;inset:0;background:#172033b8;display:none;align-items:center;justify-content:center;padding:16px;z-index:10}.modal.open{display:flex}.dialog{background:#fff;width:min(620px,100%);max-height:94vh;overflow:auto;border-radius:18px;padding:22px}.dialogHead{display:flex;justify-content:space-between;align-items:start;gap:12px}.dialog h2{margin:0}.close{border:0;background:none;font-size:25px;color:var(--muted);cursor:pointer}.privacy{font-size:12px;color:var(--muted);background:#f3f6fb;padding:10px;border-radius:8px;margin:12px 0}.profiles{display:grid;gap:9px}.profileItem{border:1px solid var(--line);border-radius:12px;padding:13px;display:flex;justify-content:space-between;align-items:center}.profileItem strong{display:block}.profileItem span{font-size:12px;color:var(--muted)}.actions{display:flex;gap:9px;margin-top:14px;flex-wrap:wrap}.formGrid{display:grid;grid-template-columns:1fr 1fr;gap:13px}.field label{font-size:13px;color:var(--muted);display:block;margin-bottom:5px}.field input,.field select,.field textarea{width:100%;border:1px solid #ccd5e3;border-radius:9px;padding:10px;background:#fff}.wide{grid-column:1/-1}.error{color:var(--red);font-size:13px;min-height:20px;margin-top:8px}.hidden{display:none!important}@media(max-width:680px){.grid{grid-template-columns:1fr 1fr}.row2{grid-template-columns:1fr}.hero{display:block}.progress{text-align:left;margin-top:18px}.formGrid{grid-template-columns:1fr}.wide{grid-column:auto}.value{font-size:23px}}
</style></head><body><div class="wrap">
<div class="top"><div class="brand">智能护膝运动中心<small id="statusText">正在连接护膝</small></div><button class="profileBtn" onclick="openProfiles()">个人档案</button></div>
<section class="hero"><div><h1>你好，<span id="userName">训练者</span></h1><p id="heroText">完成个人信息后开始个性化运动检测</p><div class="userline"><span id="bmiText">BMI --</span><span id="thresholdText">疲劳提醒线 --</span><span id="totalText">累计 0 次</span></div></div><div class="progress"><div>本次训练</div><strong><span id="sessionReps">0</span>/<span id="targetReps">10</span></strong><div class="bar"><i id="progressBar"></i></div></div></section>
<div class="grid"><div class="card"><div class="label">疲劳评分</div><div id="score" class="value">0</div></div><div class="card"><div class="label">个性化状态</div><div id="alertState" class="value warn">等待</div></div><div class="card"><div class="label">当前动作</div><div id="action" class="value">未知</div></div><div class="card"><div class="label">设备总计数</div><div id="count" class="value">0</div></div></div>
<div class="row2"><div><div class="card"><div class="label">动作质量</div><div id="quality" class="value">等待</div><div id="qualityDetail" class="detail">完成一次动作后显示结果</div></div><div class="card"><div class="label">肌电电极接触</div><div id="contact" class="value">等待</div><div id="contactDetail" class="detail">等待运动数据</div></div></div><div><div class="card advice"><div class="label">个性化建议</div><div id="advice" class="detail">请先登录个人档案</div></div><div class="card"><div class="label">原始数据</div><div id="raw" class="raw">等待数据</div><div id="age" class="detail"></div></div></div></div>
</div>
<div id="profileModal" class="modal"><div class="dialog"><div class="dialogHead"><div><h2 id="modalTitle">选择训练者</h2><div class="detail">每次护膝启动后，请先确认本次使用者。</div></div><button id="closeBtn" class="close" onclick="closeModal()">×</button></div><div class="privacy">个人档案仅保存在这台 ESP32 的本地存储中，不会上传互联网。本功能用于运动提示，不用于医疗诊断。</div>
<div id="chooser"><div id="profileList" class="profiles"></div><div class="actions"><button class="primary" onclick="showForm(-1)">新建个人档案</button></div></div>
<form id="profileForm" class="hidden" onsubmit="saveProfile(event)"><input id="profileId" type="hidden" value="-1"><div class="formGrid">
<div class="field"><label>姓名或昵称 *</label><input id="name" maxlength="24" required placeholder="例如：小明"></div><div class="field"><label>性别</label><select id="gender"><option>未设置</option><option>男</option><option>女</option></select></div>
<div class="field"><label>年龄（岁）*</label><input id="userAge" type="number" min="8" max="100" required></div><div class="field"><label>身高（cm）*</label><input id="height" type="number" min="80" max="230" required></div>
<div class="field"><label>体重（kg）*</label><input id="weight" type="number" min="20" max="250" step="0.1" required></div><div class="field"><label>运动习惯 *</label><select id="habit"><option>很少运动</option><option selected>偶尔运动</option><option>规律运动</option><option>经常运动</option></select></div>
<div class="field wide"><label>运动目标</label><select id="goal"><option>日常锻炼</option><option>膝关节康复</option><option>力量提升</option><option>体能改善</option></select></div><div class="field wide"><label>既往膝关节损伤或注意事项</label><textarea id="injury" maxlength="80" rows="3" placeholder="没有请填写“无”"></textarea></div></div>
<div id="formError" class="error"></div><div class="actions"><button class="primary" type="submit">保存并开始检测</button><button class="secondary" type="button" onclick="showChooser()">返回</button></div></form></div></div>
<script>
let profileState={active:false,profiles:[],profile:null};
let pairCode=localStorage.getItem('kneepad_pair_code')||'';
const $=id=>document.getElementById(id);
function actionText(v){return v==='SQUAT'?'深蹲':v==='DEADLIFT'?'硬拉':v==='UNKNOWN'?'未知':v;}
function qualityReasons(m){let r=[];if(m&1)r.push('动作深度不足');if(m&2)r.push('动作过快');if(m&4)r.push('下蹲与起身节奏不均');if(m&8)r.push('肌肉激活不足');if(m&16)r.push('动作过慢');if(m&32)r.push('身体不稳定');if(m&64)r.push('压力接触丢失');if(m&128)r.push('受力不均');return r.length?r.join('，'):'未发现明显问题';}
async function post(url,data){if(!pairCode){pairCode=prompt('请输入护膝设备配对码')||'';if(!pairCode)throw new Error('未输入设备配对码');localStorage.setItem('kneepad_pair_code',pairCode);}data=Object.assign({},data,{pair_code:pairCode});let r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});let j=await r.json();if(!r.ok||!j.ok){if(r.status===403){pairCode='';localStorage.removeItem('kneepad_pair_code');}throw new Error(j.error||'操作失败');}return j;}
async function loadProfileState(force=false){let r=await fetch('/api/profile');profileState=await r.json();renderProfiles();if(profileState.active){applyProfile(profileState.profile);if(force)closeModal();}else{openProfiles(true);}}
function applyProfile(p){$('userName').textContent=p.name;$('heroText').textContent=p.goal+' · '+p.exercise_habit;$('bmiText').textContent='BMI '+Number(p.bmi).toFixed(1);$('thresholdText').textContent='疲劳提醒线 '+p.fatigue_threshold;$('totalText').textContent='累计 '+p.total_reps+' 次';}
function renderProfiles(){let box=$('profileList');box.innerHTML='';if(!profileState.profiles.length){box.innerHTML='<div class="detail">还没有个人档案，请先新建。</div>';return;}profileState.profiles.forEach(p=>{let d=document.createElement('div');d.className='profileItem';d.innerHTML='<div><strong></strong><span></span></div><div class="actions"><button class="primary">使用</button><button class="secondary">编辑</button><button class="secondary">删除</button></div>';d.querySelector('strong').textContent=p.name;d.querySelector('span').textContent=p.age+' 岁 · '+p.exercise_habit;let bs=d.querySelectorAll('button');bs[0].onclick=()=>chooseProfile(p.id);bs[1].onclick=()=>showForm(p.id);bs[2].onclick=()=>deleteProfile(p.id,p.name);box.appendChild(d);});}
function openProfiles(required=false){let modal=$('profileModal'),wasOpen=modal.classList.contains('open');modal.classList.add('open');$('closeBtn').classList.toggle('hidden',required||!profileState.active);if(!wasOpen)showChooser();}
function closeModal(){if(profileState.active)$('profileModal').classList.remove('open');}
function showChooser(){$('chooser').classList.remove('hidden');$('profileForm').classList.add('hidden');$('modalTitle').textContent='选择训练者';}
function showForm(id){$('chooser').classList.add('hidden');$('profileForm').classList.remove('hidden');$('formError').textContent='';$('profileId').value=id;let p=profileState.profiles.find(x=>x.id===id);$('modalTitle').textContent=p?'编辑个人档案':'新建个人档案';$('name').value=p?p.name:'';$('gender').value=p?p.gender:'未设置';$('userAge').value=p?p.age:'';$('height').value=p?p.height_cm:'';$('weight').value=p?p.weight_kg:'';$('habit').value=p?p.exercise_habit:'偶尔运动';$('goal').value=p?p.goal:'日常锻炼';$('injury').value=p?p.injury:'无';}
async function chooseProfile(id){try{await post('/api/profile/select',{id});await loadProfileState(true);}catch(e){alert(e.message);}}
async function deleteProfile(id,name){if(!confirm('确定删除“'+name+'”的个人档案吗？此操作无法撤销。'))return;try{await post('/api/profile/delete',{id});await loadProfileState(false);showChooser();}catch(e){alert(e.message);}}
async function saveProfile(e){e.preventDefault();let data={id:$('profileId').value,name:$('name').value.trim(),gender:$('gender').value,age:$('userAge').value,height:$('height').value,weight:$('weight').value,habit:$('habit').value,goal:$('goal').value,injury:$('injury').value.trim()||'无'};try{await post('/api/profile/save',data);await loadProfileState(true);}catch(err){$('formError').textContent=err.message;}}
async function tick(){try{let r=await fetch('/api');let j=await r.json();if(!j.profile_active){openProfiles(true);return;}$('score').textContent=j.score;$('action').textContent=actionText(j.action);$('count').textContent=j.count;$('sessionReps').textContent=j.session_reps;$('targetReps').textContent=j.target_reps;$('totalText').textContent='累计 '+j.total_reps+' 次';$('progressBar').style.width=Math.min(100,j.session_reps*100/Math.max(1,j.target_reps))+'%';let tired=Number(j.score)>=Number(j.fatigue_threshold)||Number(j.alert)>0,st=$('alertState');st.textContent=tired?'建议休息':'状态良好';st.className='value '+(tired?'alert':'ok');let q=$('quality');if(j.quality_label==='WAIT'){q.textContent='等待';q.className='value';$('qualityDetail').textContent='完成一次动作后显示结果';}else{q.textContent=j.quality+' / 100 '+j.quality_label;q.className='value '+(j.quality_label==='GOOD'?'ok':j.quality_label==='WRONG'?'alert':'warn');$('qualityDetail').textContent=qualityReasons(j.quality_fail_mask);}let bad=['LOW','SAT','NOISY'].includes(j.contact),c=$('contact');c.textContent=bad?'请检查':j.contact==='OK'?'接触良好':'等待';c.className='value '+(bad?'alert':j.contact==='OK'?'ok':'warn');$('contactDetail').textContent=j.contact==='LOW'?'信号过低':j.contact==='SAT'?'信号饱和':j.contact==='NOISY'?'噪声异常':j.contact==='OK'?'电极信号正常':'等待动作数据';$('advice').textContent=j.advice;$('raw').textContent=j.raw;$('age').textContent='数据延迟 '+j.age_ms+' ms';$('statusText').textContent=j.age_ms>2500?'护膝数据未连接':'护膝数据已连接';}catch(e){$('statusText').textContent='网页连接异常';}}
loadProfileState();setInterval(tick,500);tick();
</script></body></html>
)KNEEHTML";
  server.send(200, "text/html; charset=utf-8", html);
}

static String compactProfileJson(uint8_t id, const UserProfile &p)
{
  String body = "{";
  body += "\"id\":" + String(id) + ",";
  body += "\"name\":\"" + jsonEscape(p.name) + "\",";
  body += "\"gender\":\"" + jsonEscape(p.gender) + "\",";
  body += "\"age\":" + String(p.age) + ",";
  body += "\"height_cm\":" + String(p.heightCm) + ",";
  body += "\"weight_kg\":" + String(p.weightX10 / 10.0f, 1) + ",";
  body += "\"exercise_habit\":\"" + jsonEscape(p.exerciseHabit) + "\",";
  body += "\"goal\":\"" + jsonEscape(p.goal) + "\",";
  body += "\"injury\":\"" + jsonEscape(p.injury) + "\",";
  body += "\"total_reps\":" + String(p.totalReps) + ",";
  body += "\"bmi\":" + String(profileBmi(p), 1) + ",";
  body += "\"fatigue_threshold\":" + String(personalizedFatigueThreshold(p)) + ",";
  body += "\"target_reps\":" + String(personalizedTargetReps(p));
  body += "}";
  return body;
}

static void sendJsonError(int code, const String &message)
{
  server.send(code, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}");
}

static void handleProfileState()
{
  String body = "{\"active\":";
  body += activeProfileIndex >= 0 ? "true" : "false";
  body += ",\"profiles\":[";
  uint8_t count = profileCount();
  bool first = true;
  for (uint8_t i = 0; i < count; ++i) {
    UserProfile p;
    if (!loadProfile(i, p)) continue;
    if (!first) body += ',';
    first = false;
    body += compactProfileJson(i, p);
  }
  body += "],\"profile\":";
  if (activeProfileIndex >= 0) body += compactProfileJson((uint8_t)activeProfileIndex, activeProfile);
  else body += "null";
  body += "}";
  server.send(200, "application/json; charset=utf-8", body);
}

static void handlePairVerify()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "设备配对码错误");
    return;
  }
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"device\":\"KneePad_ESP32\"}");
}

static void handleProfileSelect()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "请先输入正确的设备配对码");
    return;
  }
  if (trainingActive) {
    sendJsonError(409, "请先结束当前训练再切换使用者");
    return;
  }
  int id = server.arg("id").toInt();
  UserProfile profile;
  if (id < 0 || id >= profileCount() || !loadProfile((uint8_t)id, profile)) {
    sendJsonError(404, "未找到这个个人档案");
    return;
  }
  selectProfile((uint8_t)id);
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

static String cleanField(const String &value, size_t maxLength)
{
  String result = value;
  result.trim();
  if (result.length() > maxLength) result.remove(maxLength);
  return result;
}

static void handleProfileSave()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "请先输入正确的设备配对码");
    return;
  }
  if (trainingActive) {
    sendJsonError(409, "请先结束当前训练再修改档案");
    return;
  }
  UserProfile profile;
  profile.name = cleanField(server.arg("name"), 24);
  profile.gender = cleanField(server.arg("gender"), 8);
  profile.age = (uint16_t)server.arg("age").toInt();
  profile.heightCm = (uint16_t)server.arg("height").toInt();
  float weight = server.arg("weight").toFloat();
  profile.weightX10 = (uint16_t)(weight * 10.0f + 0.5f);
  profile.exerciseHabit = cleanField(server.arg("habit"), 16);
  profile.goal = cleanField(server.arg("goal"), 20);
  profile.injury = cleanField(server.arg("injury"), 80);
  if (profile.injury.length() == 0) profile.injury = "无";

  if (profile.name.length() == 0 || profile.age < 8 || profile.age > 100 ||
      profile.heightCm < 80 || profile.heightCm > 230 || weight < 20.0f || weight > 250.0f) {
    sendJsonError(400, "请检查姓名、年龄、身高和体重是否填写正确");
    return;
  }

  int id = server.arg("id").toInt();
  uint8_t count = profileCount();
  if (id < 0) {
    if (count >= MAX_PROFILES) {
      sendJsonError(409, "最多只能保存 6 个个人档案");
      return;
    }
    id = count;
    profile.totalReps = 0;
    preferences.putUChar("pcount", count + 1);
  } else {
    UserProfile oldProfile;
    if (id >= count || !loadProfile((uint8_t)id, oldProfile)) {
      sendJsonError(404, "未找到需要编辑的个人档案");
      return;
    }
    profile.totalReps = oldProfile.totalReps;
  }

  saveProfile((uint8_t)id, profile);
  selectProfile((uint8_t)id);
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"id\":" + String(id) + "}");
}

static void handleProfileDelete()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "请先输入正确的设备配对码");
    return;
  }
  if (trainingActive) {
    sendJsonError(409, "请先结束当前训练再删除档案");
    return;
  }
  int id = server.arg("id").toInt();
  uint8_t count = profileCount();
  if (id < 0 || id >= count) {
    sendJsonError(404, "未找到需要删除的个人档案");
    return;
  }

  flushActiveProfile();
  for (uint8_t i = (uint8_t)id; i + 1 < count; ++i) {
    UserProfile next;
    if (loadProfile(i + 1, next)) saveProfile(i, next);
  }
  if (count > 0) clearProfile(count - 1);
  preferences.putUChar("pcount", count > 0 ? count - 1 : 0);

  if (activeProfileIndex == id) {
    activeProfileIndex = -1;
    sessionReps = 0;
    trainingActive = false;
  } else if (activeProfileIndex > id) {
    activeProfileIndex--;
  }
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

static void handleTrainingStart()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "请先输入正确的设备配对码");
    return;
  }
  if (activeProfileIndex < 0) {
    sendJsonError(409, "请先选择个人档案");
    return;
  }
  trainingActive = true;
  trainingStartedMs = millis();
  trainingStartDeviceCount = latestCount;
  sessionReps = 0;
  sessionLastDeviceCount = latestCount;
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"started\":true}");
}

static void handleTrainingEnd()
{
  if (!pairCodeValid()) {
    sendJsonError(403, "请先输入正确的设备配对码");
    return;
  }
  if (!trainingActive) {
    sendJsonError(409, "当前没有正在进行的训练");
    return;
  }
  uint32_t durationMs = millis() - trainingStartedMs;
  uint32_t reps = latestCount >= trainingStartDeviceCount ? latestCount - trainingStartDeviceCount : sessionReps;
  trainingActive = false;
  flushActiveProfile();
  String body = "{\"ok\":true,\"ended\":true,\"duration_ms\":" + String(durationMs) +
                ",\"reps\":" + String(reps) + "}";
  server.send(200, "application/json; charset=utf-8", body);
}

static void handleApi()
{
  uint32_t age = latestUpdateMs ? (millis() - latestUpdateMs) : 0xFFFFFFFFUL;
  String body = "{";
  body += "\"profile_active\":";
  body += activeProfileIndex >= 0 ? "true," : "false,";
  body += "\"training_active\":";
  body += trainingActive ? "true," : "false,";
  body += "\"score\":" + String(latestScore) + ",";
  body += "\"action\":\"" + jsonEscape(latestAction) + "\",";
  body += "\"count\":" + String(latestCount) + ",";
  body += "\"alert\":" + String(latestAlert) + ",";
  body += "\"quality\":" + String(latestQuality) + ",";
  body += "\"quality_label\":\"" + jsonEscape(latestQualityLabel) + "\",";
  body += "\"quality_fail_mask\":" + String(latestQualityFailMask) + ",";
  body += "\"contact\":\"" + jsonEscape(latestContact) + "\",";
  body += "\"walk_count\":" + String(latestWalkCount) + ",";
  body += "\"squat_count\":" + String(latestSquatCount) + ",";
  body += "\"deadlift_count\":" + String(latestDeadliftCount) + ",";
  uint32_t pressAge = latestPressMs ? (millis() - latestPressMs) : 0xFFFFFFFFUL;
  body += "\"press_med\":" + String(latestPressMed) + ",";
  body += "\"press_lat\":" + String(latestPressLat) + ",";
  body += "\"press_diff\":" + String(latestPressDiff) + ",";
  body += "\"press_status\":\"" + jsonEscape(latestPressStatus) + "\",";
  body += "\"press_age_ms\":" + String(pressAge) + ",";
  body += "\"age_ms\":" + String(age) + ",";
  body += "\"raw\":\"" + jsonEscape(latestLine) + "\",";
  if (activeProfileIndex >= 0) {
    body += "\"user_name\":\"" + jsonEscape(activeProfile.name) + "\",";
    body += "\"fatigue_threshold\":" + String(personalizedFatigueThreshold(activeProfile)) + ",";
    body += "\"target_reps\":" + String(personalizedTargetReps(activeProfile)) + ",";
    body += "\"session_reps\":" + String(sessionReps) + ",";
    body += "\"total_reps\":" + String(activeProfile.totalReps) + ",";
    body += "\"advice\":\"" + jsonEscape(personalizedAdvice()) + "\"";
  } else {
    body += "\"fatigue_threshold\":70,\"target_reps\":0,\"session_reps\":0,\"total_reps\":0,";
    body += "\"advice\":\"请先选择或创建个人档案\"";
  }
  body += "}";
  server.send(200, "application/json; charset=utf-8", body);
}

static void requestTelemetryStreams()
{
  // The commands are harmless when a stream is already enabled and recover
  // automatically when the STM32 boots later than the ESP32 or is reset.
  Stm32Serial.print("knee_start\r\n");
  Stm32Serial.print("pressure_stream on\r\n");
  telemetryStreamAskedMs = millis();
}

void setup()
{
  Serial.begin(115200);
  Stm32Serial.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
  preferences.begin("kneepad", false);

  delay(1500);
  requestTelemetryStreams();

  // activeProfileIndex intentionally stays -1 after every reboot so each new
  // knee-pad session asks the wearer to identify themselves before monitoring.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/api/pair/verify", HTTP_POST, handlePairVerify);
  server.on("/api/profile", HTTP_GET, handleProfileState);
  server.on("/api/profile/select", HTTP_POST, handleProfileSelect);
  server.on("/api/profile/save", HTTP_POST, handleProfileSave);
  server.on("/api/profile/delete", HTTP_POST, handleProfileDelete);
  server.on("/api/training/start", HTTP_POST, handleTrainingStart);
  server.on("/api/training/end", HTTP_POST, handleTrainingEnd);
  server.begin();

  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Personal profile login is required for every new boot.");
}

void loop()
{
  consumeSerial(Stm32Serial, rxLine);
  consumeSerial(Serial, debugRxLine);
  server.handleClient();
  if (profileDirty && millis() - profileDirtySinceMs >= 5000) flushActiveProfile();

  bool kneeStale = (latestUpdateMs == 0) || (millis() - latestUpdateMs > 3000);
  bool pressStale = (latestPressMs == 0) || (millis() - latestPressMs > 3000);
  if ((kneeStale || pressStale) && millis() - telemetryStreamAskedMs > 3000) {
    requestTelemetryStreams();
  }
}
