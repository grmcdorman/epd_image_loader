/*
 *
 * Master repository for Waveshare drivers: https://github.com/waveshare/e-Paper/tree/master/Arduino
 *
 * The code in the 'epd' directory is almost verbatim from there, with the following changes:
 * - In all font.c files, the following replaces `#include <avr/pgmspace.h>`:
 *     #ifdef ESP8266
 *     #include <avr/pgmspace.h>
 *     #else
 *     #define PROGMEM
 *     #endif
 *
 * - epd1in54_v2.cpp has `pgm_read_byte` changed to `generic_pgm_read_byte` throughout
 * - epd1n54_v2.cpp has the following added at the beginning:
 *
 *  #ifdef ESP8266
 *  #include <avr/pgmspace.h>
 *  #else
 *  #define PROGMEM
 *  #endif
 *
 *  #if defined(ESP8266)
 *  // Flash constant support.
 *  static inline uint8_t generic_pgm_read_byte(const void* addr) { return pgm_read_byte(addr); }
 *  #else
 *  static inline uint8_t generic_pgm_read_byte(const void* addr) { return *reinterpret_cast<const uint8_t *>(addr); }
 *  #endif
 *
 * Reference for the Waveshare display, the 1.54" black/white model: https://www.waveshare.com/wiki/1.54inch_e-Paper_Module
 * See also https://www.waveshare.com/w/upload/e/e5/1.54inch_e-paper_V2_Datasheet.pdf
 * Note that, during operation, Waveshare recommends a full display reset at least every 24 hours. This will cause
 * the display to flash and clear.
 *
 * Connections from Waveshare display, Waveshare designation, connection direction, and wire colours on my display:
 *          ESP8266  ESP32
 *  * BUSY  D2      19           INPUT   Purple
 *  * RST   D1      18           OUTPUT  White       Reset
 *  * DC    D3      23     D/C#  OUTPUT  Green       Data/Command (high: data; low: command). Warning: may fail boot if pulled low.
 *  * CS    D8       4     CS#   OUTPUT  Orange      Chip select; when low chip accepts data on DC. Warning: may fail boot if pulled high.
 *  * CLK   D5      22     SCL   OUTPUT  Yellow      Serial clock
 *  * DIN   D7      21     SDA   OUTPUT  Blue        Serial data
 *  * GND   GND                  N/A     Black
 *  * VCC   3.3V                 N/A     Red
 *
 * It is possible to shuffle some assignements, e.g., with appropriate code,
 * to use the RX line (with a prior call to `pinMode(RX, FUNCITON_3)`), but
 * my recommendation is to go with the above assignments.
 *
 * GPIO 6 through 7 on ESP32-WROOM-32 are not available for use; the original
 * Waveshare code used 7 through 9 for BUSY, RST, and DC respectively. I have modified
 * these assignments as listed above, pins on the same side as the SCL/SDA pins.
 * There does not seem to be a particular reason other pins couldn't be used.
 *
 * Other references for ESP32 suggest D31 = MOSI, D19 = MISO, D16 = SCLK (SCL), D5 = CS.
 * This does not seem to be consistent with documentation that clearly gives SCL as GPIO22
 * and SDA as GPIO21.
 * A soldered-in surface mount jumper (a "0 ohm" resistor) on the board can be moved to
 * change the device to a 3-line SPI. Waveshare documents indicate that DC must be
 * connected to ground in this mode. The Waveshare code does not support this mode;
 * however, basically there is one additional leading bit transferred in each
 * sequence which, when 0, indicates a command, and when 1 indicates a data byte.
 *
 * Waveshare documents also imply that data can be read from the display; the data is presented
 * on the DIN line, i.e. a bi-directional line. The Waveshare-provided code does not use this
 * functionality.
 *
 *
 * The `epdif.h` file has been modified to match these connections for both ESP8266 and ESP32.
 *
 * CLK (D5) and DIN (D7) are used for the default SPI functionality, and are not directly referenced
 * by the Waveshare code. These connections are required.
 *
 * If there are no other SPI devices attached, the CS connection can be
 * pulled to ground and the usage of the CS_PIN in the code removed.
 *
 * On ESP8266, do not connect the display to D4; some online pages suggest using this for one of the connections.
 * D4 is connected to the on-board LED, and the boot will fail if it is pulled low. Pulling
 * it low also lights the on-board LED.
 *
 * The Waveshare displays do not have any read capability
 */

#include <Ticker.h>

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#include <esp_random.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESP8266TrueRandom.h>
#endif


#include <ESPAsyncWebServer.h>
// Conflicting declarations. I wish people would use namespaces.
#define HTTP_ANY WIFIMANGER_HTTP_ANY
#define HTTP_GET WIFIMANGER_HTTP_GET
#define HTTP_HEAD WIFIMANGER_HTTP_HEAD
#define HTTP_POST WIFIMANGER_HTTP_POST
#define HTTP_PUT WIFIMANGER_HTTP_PUT
#define HTTP_PATCH WIFIMANGER_HTTP_PATCH
#define HTTP_DELETE WIFIMANGER_HTTP_DELETE
#define HTTP_OPTIONS WIFIMANGER_HTTP_OPTIONS
#include <WiFiManager.h>
#undef HTTP_ANY
#undef HTTP_GET
#undef HTTP_HEAD
#undef HTTP_POST
#undef HTTP_PUT
#undef HTTP_PATCH
#undef HTTP_DELETE
#undef HTTP_OPTIONS

#ifndef ESP8266
#include <PNGdec.h>
#endif
#include <qrcode.h>

// The standard ESP32 toolkit does not include LittleFS. There
// is a library that does.
#include <LittleFS.h>

static constexpr int BLACK     = 0;
static constexpr int WHITE     = 1;

#include "epd/epd1in54_V2.h"
#include "epd/epdpaint.h"
#include "epd/fonts.h"

static Epd epd;

// The display size, in pixels.
static constexpr size_t image_width = 200;
static constexpr size_t image_height = 200;

//!< This must be sufficiently large to hold the image width x height, in bits;
//!< in other words, (width * height / 8) bytes. The 200x200 resolution works
// out to exactly 5,000 bytes.
static unsigned char image[image_width * image_height / 8];
static Paint paint(image, image_width, image_height);
static String currentImage{"<none>"};
static String epdState{"Powered"};


static String qr_code_text;
static int qr_code_version{0};
static int qr_code_ecc{0};
static bool qr_code_scale{false};

static AsyncWebServer server(80);

static char password[64] = "PassWord348";

static const char index_html[] PROGMEM =
    "<!DOCTYPE HTML>"
    "<html lang=\"en\">"
    "<head>"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "  <meta charset=\"UTF-8\">"
    "</head>"
    "<script language=\"javascript\">"
    "function _(el) {\n"
    "  return document.getElementById(el);\n"
    "}\n"
    "function recalcSize()\n"
    "{\n"
    "    var version = parseInt(_(\"version\").value);\n"
    "    var ecc = parseInt(_(\"ecc\").value);\n"
    "    var ecc_sizes;\n"
    // Alphanumeric only is upper case, numbers, space and '$%*+-./:',
    // which implies an *upper case* URL could fit in smaller codes.
    "    switch(version) {\n"
    "    case 1: ecc_sizes = [[41, 25, 17], [34, 20, 14], [27, 16, 11], [17, 10, 7]]; break;\n"
    "    case 2: ecc_sizes = [[77, 47, 32], [63, 38, 26], [48, 29, 20], [34, 20, 14]]; break;\n"
    "    case 3: ecc_sizes = [[127, 77, 53], [101, 61, 42], [77, 47, 32], [58, 35, 24]]; break;\n"
    "    case 4: ecc_sizes = [[187, 114, 78], [149, 90, 62], [111, 67, 46], [82, 50, 34]]; break;\n"
    "    case 5: ecc_sizes = [[2555, 154, 106], [202, 122, 84], [144, 87, 60], [106, 64, 44]]; break;\n"
    "    case 6: ecc_sizes = [[322, 195, 134], [255, 154, 106], [178, 108, 74], [139, 84, 58]]; break;\n"
    "    case 7: ecc_sizes = [[370, 224, 154], [293, 178, 122], [207, 125, 86], [154, 93, 64]]; break;\n"
    "    case 8: ecc_sizes = [[461, 279, 192], [365, 221, 152], [259, 157, 108], [202, 122, 84]]; break;\n"
    "    case 9: ecc_sizes = [[552, 335, 230], [432, 262, 180], [312, 189, 130], [235, 143, 98]]; break;\n"
    "    case 10: ecc_sizes = [[652, 395, 271], [513, 311, 213], [364, 221, 151], [288, 174, 119]]; break;\n"
    "    case 11: ecc_sizes = [[772, 468, 321], [604, 366, 251], [427, 259, 177], [331, 200, 137]]; break;\n"
    "    case 12: ecc_sizes = [[883, 535, 367], [691, 419, 287], [489, 296, 203], [374, 227, 155]]; break;\n"
    "    case 13: ecc_sizes = [[1022, 619, 425], [796, 483, 331], [580, 352, 241], [427, 259, 177]]; break;\n"
    "    case 14: ecc_sizes = [[1101, 667, 458], [871, 528, 362], [621, 376, 258], [468, 283, 194]]; break;\n"
    "    case 15: ecc_sizes = [[1250, 758, 520], [991, 600, 412], [703, 426, 292], [530, 321, 220]]; break;\n"
    "    case 16: ecc_sizes = [[1408, 854, 586], [1082, 656, 450], [775, 470, 322], [602, 365, 250]]; break;\n"
    "    case 17: ecc_sizes = [[1548, 938, 644], [1212, 734, 504], [876, 531, 364], [674, 408, 280]]; break;\n"
    "    case 18: ecc_sizes = [[1725, 1046, 718], [1346, 816, 560], [948, 574, 394], [746, 452, 310]]; break;\n"
    "    case 19: ecc_sizes = [[1903, 1153, 792], [1500, 909, 624], [1063, 644, 442], [813, 493, 338]]; break;\n"
    "    case 20: ecc_sizes = [[2061, 1249, 858], [1600, 970, 666], [1159, 702, 482], [919, 557, 382]]; break;\n"
    "    case 21: ecc_sizes = [[2232, 1352, 929], [1708, 1035, 711], [1224, 742, 509], [969, 587, 403]]; break;\n"
    "    case 22: ecc_sizes = [[2409, 1460, 1003], [1872, 1134, 779], [1358, 823, 565], [1056, 640, 439]]; break;\n"
    "    case 23: ecc_sizes = [[2620, 1588, 1091], [2059, 1248, 857], [1468, 890, 611], [1108, 672, 461]]; break;\n"
    "    case 24: ecc_sizes = [[2812, 1704, 1171], [2188, 1326, 911], [1588, 963, 661], [1228, 744, 511]]; break;\n"
    "    case 25: ecc_sizes = [[3057, 1853, 1273], [2395, 1451, 997], [1718, 1041, 715], [1286, 779, 535]]; break;\n"
    "    case 26: ecc_sizes = [[3283, 1990, 1367], [2544, 1542, 1059], [1804, 1094, 751], [1425, 864, 593]]; break;\n"
    "    case 27: ecc_sizes = [[3517, 2132, 1465], [2701, 1637, 1125], [1933, 1172, 805], [1501, 910, 625]]; break;\n"
    "    case 28: ecc_sizes = [[3669, 2223, 1528], [2857, 1732, 1190], [2085, 1263, 868], [1581, 958, 658]]; break;\n"
    "    case 29: ecc_sizes = [[3909, 2369, 1628], [3035, 1839, 1264], [2181, 1322, 908], [1677, 1016, 698]]; break;\n"
    "    case 30: ecc_sizes = [[4158, 2520, 1732], [3289, 1994, 1370], [2358, 1429, 982], [1782, 1080, 742]]; break;\n"
    "    case 31: ecc_sizes = [[4417, 2677, 1840], [3486, 2113, 1452], [2473, 1499, 1030], [1897, 1150, 790]]; break;\n"
    "    case 32: ecc_sizes = [[4686, 2840, 1952], [3693, 2238, 1538], [2670, 1618, 1112], [2022, 1226, 842]]; break;\n"
    "    case 33: ecc_sizes = [[4965, 3009, 2068], [3909, 2369, 1628], [2805, 1700, 1168], [2157, 1307, 898]]; break;\n"
    "    case 34: ecc_sizes = [[5253, 3183, 2188], [4134, 2506, 1722], [2949, 1787, 1228], [2301, 1394, 958]]; break;\n"
    "    case 35: ecc_sizes = [[5529, 3351, 2303], [4343, 2632, 1809], [3081, 1867, 1283], [2361, 1431, 983]]; break;\n"
    "    case 36: ecc_sizes = [[5836, 3537, 2431], [4588, 2780, 1911], [3244, 1966, 1351], [2524, 1530, 1051]]; break;\n"
    "    case 37: ecc_sizes = [[6153, 3729, 2563], [4775, 2894, 1989], [3417, 2071, 1423], [2625, 1591, 1093]]; break;\n"
    "    case 38: ecc_sizes = [[6479, 3927, 2699], [5039, 3054, 2099], [3599, 2181, 1499], [2735, 1658, 1139]]; break;\n"
    "    case 39: ecc_sizes = [[6743, 4087, 2809], [5313, 3220, 2213], [3791, 2298, 1579], [2927, 1774, 1219]]; break;\n"
    "    case 40: ecc_sizes = [[7089, 4296, 2953], [5596, 3391, 2331], [3993, 2420, 1663], [3057, 1852, 1273]]; break;\n"
    "    }\n"
    "    _(\"size\").innerText = ecc_sizes[ecc];\n"
    "    checkSize();\n"
    "}\n"
    "function checkSize()\n"
    "{\n"
    "    var textValue = _(\"text\").value, size = _(\"size\"), generate = _(\"generate\"),\n"
    "       maxLength = JSON.parse(\"[\" + size.innerText + \"]\")[/^[0-9]*$/.test(textValue) ? 0 : /^[A-Z0-9 $%%*+-.\\/:]*$/.test(textValue) ? 1 : 2];\n"
    "    generate.disabled = textValue.length > maxLength;\n"
    "}\n"
    "function updateStatus() {\n"
    "  xmlhttp=new XMLHttpRequest();\n"
    "  xmlhttp.open(\"GET\", \"/state\", false);\n"
    "  xmlhttp.onload = function() {\n"
    "   var statusData = JSON.parse(xmlhttp.responseText);\n"
    "   _(\"currentimage\").innerText = statusData.currentImage;\n"
    "   _(\"epdstate\").innerText = statusData.epdstate;\n"
    "   _(\"freestorage\").innerText = statusData.freestorage;\n"
    "   _(\"usedstorage\").innerText = statusData.usedstorage;\n"
    "   _(\"totalstorage\").innerText = statusData.totalstorage;\n"
    "  };"
    "  xmlhttp.send();\n"
    "}\n"
    "function sleepButton()\n"
    "{\n"
    "   xmlhttp=new XMLHttpRequest();\n"
    "   xmlhttp.open(\"GET\", \"/sleep\");\n"
    "  xmlhttp.onload = function() {\n"
    "    updateStatus();"
    "  };\n"
    "  xmlhttp.send();\n"
    "}\n"
    "function clearDisplayButton()\n"
    "{\n"
    "   xmlhttp=new XMLHttpRequest();\n"
    "   xmlhttp.open(\"GET\", \"/clear\");\n"
    "  xmlhttp.onload = function() {\n"
    "    updateStatus();\n"
    "  };\n"
    "  xmlhttp.send();\n"
    "}\n"

    // Following code modified from https://github.com/smford/esp32-asyncwebserver-fileupload-example/blob/master/example-02/webpages.h
    "function deleteButton(filename)\n"
    "{\n"
    "   xmlhttp=new XMLHttpRequest();\n"
    "   xmlhttp.open(\"GET\", \"/delete?file=\" + filename, false);\n"
    "   xmlhttp.send();\n"
    "   _(\"status\").innerText = xmlhttp.responseText;\n"
    "   listFilesButton();"
    "}"
    "function listFilesButton() {\n"
    "  xmlhttp=new XMLHttpRequest();\n"
    "  xmlhttp.open(\"GET\", \"/listfiles\", false);\n"
    "  xmlhttp.send();\n"
    "  _(\"detailsheader\").innerHTML = \"<h3>Files<h3>\";\n"
    "  _(\"details\").innerHTML = xmlhttp.responseText;\n"
    "  updateStatus();"
    "}\n"
    "function showUploadButtonFancy() {\n"
    "  _(\"detailsheader\").innerHTML = \"<h3>Upload File<h3>\"\n"
    "  _(\"status\").innerHTML = \"\";\n"
    "  var uploadform = \"<form method = \\\"POST\\\" action = \\\"/\\\" enctype=\\\"multipart/form-data\\\"><input type=\\\"file\\\" name=\\\"data\\\"/><input type=\\\"submit\\\" name=\\\"upload\\\" value=\\\"Upload\\\" title = \\\"Upload File\\\"></form>\"\n"
    "  _(\"details\").innerHTML = uploadform;\n"
    "  var uploadform =\n"
    "  \"<form id=\\\"upload_form\\\" enctype=\\\"multipart/form-data\\\" method=\\\"post\\\">\" +\n"
    "  \"<input type=\\\"file\\\" name=\\\"file1\\\" id=\\\"file1\\\" onchange=\\\"uploadFile()\\\"><br>\" +\n"
    "  \"<progress id=\\\"progressBar\\\" value=\\\"0\\\" max=\\\"100\\\" style=\\\"width:300px;\\\"></progress>\" +\n"
    "  \"<h3 id=\\\"status\\\"></h3>\" +\n"
    "  \"<p id=\\\"loaded_n_total\\\"></p>\" +\n"
    "  \"</form>\";\n"
    "  _(\"details\").innerHTML = uploadform;\n"
    "}\n"
    "function uploadFile() {\n"
    "  var file = _(\"file1\").files[0];\n"
    "  // alert(file.name+\" | \"+file.size+\" | \"+file.type);\n"
    "  var formdata = new FormData();\n"
    "  formdata.append(\"file1\", file);\n"
    "  var ajax = new XMLHttpRequest();\n"
    "  ajax.upload.addEventListener(\"progress\", progressHandler, false);\n"
    "  ajax.addEventListener(\"load\", completeHandler, false); // doesnt appear to ever get called even upon success\n"
    "  ajax.addEventListener(\"error\", errorHandler, false);\n"
    "  ajax.addEventListener(\"abort\", abortHandler, false);\n"
    "  ajax.open(\"POST\", \"/\");\n"
    "  ajax.send(formdata);\n"
    "}\n"
    "function progressHandler(event) {\n"
    "  //_(\"loaded_n_total\").innerHTML = \"Uploaded \" + event.loaded + \" bytes of \" + event.total; // event.total doesnt show accurate total file size\n"
    "  _(\"loaded_n_total\").innerHTML = \"Uploaded \" + event.loaded + \" bytes\";\n"
    "  var percent = (event.loaded / event.total) * 100;\n"
    "  _(\"progressBar\").value = Math.round(percent);\n"
    "  _(\"status\").innerHTML = Math.round(percent) + \"%% uploaded... please wait\";\n"
    "  if (percent >= 100) {\n"
    "    _(\"status\").innerHTML = \"Please wait, writing file to filesystem\";\n"
    "  }\n"
    "}\n"
    "function completeHandler(event) {\n"
    "  _(\"status\").innerHTML = \"Upload Complete\";\n"
    "  _(\"progressBar\").value = 0;\n"
    "  xmlhttp=new XMLHttpRequest();\n"
    "  xmlhttp.open(\"GET\", \"/listfiles\", false);\n"
    "  xmlhttp.send();\n"
    "  _(\"status\").innerHTML = \"File Uploaded\";\n"
    "  _(\"detailsheader\").innerHTML = \"<h3>Files<h3>\";\n"
    "  _(\"details\").innerText = xmlhttp.responseText;\n"
    "  updateStatus();"
    "}\n"
    "function errorHandler(event) {\n"
    "  _(\"status\").innerHTML = \"Upload Failed\";\n"
    "}\n"
    "function abortHandler(event) {\n"
    "  _(\"status\").innerHTML = \"Upload Aborted\";\n"
    "}\n"
    "</script>"
    "<body onload=\"listFilesButton()\">"
    "  <h1>Status</h1>"
    "  <p>Free Storage: <span id=\"freestorage\">%FREESPIFFS%</span> | Used Storage: <span id=\"usedstorage\">%USEDSPIFFS%</span> | Total Storage: <span id=\"totalstorage\">%TOTALSPIFFS%</span> | Current image: <span id=\"currentimage\">%CURRENTIMAGE%</span> | State: <span id=\"epdstate\">%EPDSTATE%</span></p>"
    "  <h1>QR Code Generation</h1>"
#ifdef ESP8266
    "   <h2>WARNING: Large versions (typically around 17 or higher) will cause watchdog timer resets on ESP8266.</h2>"
#endif
    "  <form method=\"POST\" action=\"/qr\">"
    "   <input type=\"text\" name=\"text\" id=\"text\"/ onchange=\"checkSize()\" oninput=\"checkSize()\">"
    "   <label for=\"version\">QR version:</label>"
    "   <select name=\"version\" id=\"version\" onchange=\"recalcSize()\">"
    "      <option value=\"1\">1</option>"
    "      <option value=\"2\">2</option>"
    "      <option value=\"3\">3</option>"
    "      <option value=\"4\" selected>4</option>"
    "      <option value=\"5\">5</option>"
    "      <option value=\"6\">6</option>"
    "      <option value=\"7\">7</option>"
    "      <option value=\"8\">8</option>"
    "      <option value=\"9\">9</option>"
    "      <option value=\"10\">10</option>"
    "      <option value=\"11\">11</option>"
    "      <option value=\"12\">12</option>"
    "      <option value=\"13\">13</option>"
    "      <option value=\"14\">14</option>"
    "      <option value=\"15\">15</option>"
    "      <option value=\"16\">16</option>"
    "      <option value=\"17\">17</option>"
    "      <option value=\"18\">18</option>"
    "      <option value=\"19\">19</option>"
    "      <option value=\"20\">20</option>"
    "      <option value=\"21\">21</option>"
    "      <option value=\"22\">22</option>"
    "      <option value=\"23\">23</option>"
    "      <option value=\"24\">24</option>"
    "      <option value=\"25\">25</option>"
    "      <option value=\"26\">26</option>"
    "      <option value=\"27\">27</option>"
    "      <option value=\"28\">28</option>"
    "      <option value=\"29\">29</option>"
    "      <option value=\"30\">30</option>"
    "      <option value=\"31\">31</option>"
    "      <option value=\"32\">32</option>"
    "      <option value=\"33\">33</option>"
    "      <option value=\"34\">34</option>"
    "      <option value=\"35\">35</option>"
    "      <option value=\"36\">36</option>"
    "      <option value=\"37\">37</option>"
    "      <option value=\"38\">38</option>"
    "      <option value=\"39\">39</option>"
    "      <option value=\"40\">40</option>"
    "   </select> "
    "   <label for=\"ecc\">EEC:</label>"
    "   <select name=\"ecc\" id=\"ecc\" onchange=\"recalcSize()\">"
    "     <option value=\"0\">Low</option>"
    "     <option value=\"1\">Medium</option>"
    "     <option value=\"2\">Quartile</option>"
    "     <option value=\"3\" selected>High</option>"
    "   </select> "
    "   <label for=\"scale\">Scale image to fit:</label>"
    "   <input type=\"radio\" name=\"scale\" value=\"scale\" title=\"Scale to fit\" checked=\"true\">"
    "   <input type=\"submit\" id=\"generate\" name=\"generate\" value=\"Generate\" title=\"Generate QR\">"
    "   Maximum lengths (numeric, alphanumeric, others): <span id=\"size\"> 139,84,58 </span>"
    "   <br> Maximum lengths are for numeric only, <em>upper</em> case alphanumeric, <b>$%%*+-./:</b> characters and space, and finally for general data."
    "   <br>Generation is asynchronous; refresh the file list shortly after the QR code is shown on the display."
    "   </form>"
    "  <h1>Display Control</h1>"
    "  <p><button onclick=\"sleepButton()\">Sleep E-Ink</button>"
    "  <button onclick=\"clearDisplayButton()\">Clear Display</button><br>"
    "   Power can be turned off without corrupting a sleeping display; otherwise corruption may occur.<br>"
    "   <b>Note: Do not set display to sleep for long-term storage with an image shown.</b>"
    "  <p><h1>File Upload</h1></p>"
    "  <button onclick=\"showUploadButtonFancy()\">Upload File</button>"
    "  <button onclick=\"listFilesButton()\">List Files</button>"
    "  <div id=\"status\"></div>"
    "  <div id=\"detailsheader\" style=\"font-size: medium; font-weight: bold\">Files</div>"
    "  <div id=\"details\">%FILELIST%</div>"
    "</body>"
    "</html>";
//////////////////////////////////////////////////////////////////////////
static void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static String processor(const String& var);
// Make size of files human readable
// source: https://github.com/CelliesProjects/minimalUploadAuthESP32
static String humanReadableSize(const size_t bytes);
static String listFiles(bool ishtml);
static void display_image(const String *filename);
static void snapshot(Paint &snapshotPaint);
static void display_qr_code();


/**
 * @brief Display a message in the paint
 *
 * This will of course be later copied to the display.
 * @param offset Vertical offset of message.
 * @param font   Font to use to display message.
 * @param item   Message to display.
 */
static inline void display_status_message(int offset, sFONT &font, const char *item)
{
    paint.DrawStringAt(0, offset, item, &font, BLACK);
}

/**
 * @brief Display a list of messages.
 *
 * Messages displayed vertically, each line offset by the font height.
 * Messages after the first are displayed in the Font16 font.
 *
 * @tparam Args  Argument type(s).
 * @param offset Offset of first line
 * @param font   Font for first message.
 * @param item   First message.
 * @param args   Additional messages.
 */
template<typename...Args>
static void display_status_message(int offset, sFONT &font, const char *item, Args...args)
{
    display_status_message(offset, font, item);
    display_status_message(offset + font.Height, Font16, args...);
}

/**
 * @brief Display a list of messsages from the screen top.
 *
 * The first message is displayed in Font24, subsequent messages
 * are displayed in Font16.
 *
 * @tparam Args  Argument type(s).
 * @param item   First message.
 * @param args   Subsequent messages.
 */
template<typename...Args>
static void display_status_message(const char *item, Args...args)
{
    paint.SetWidth(image_width);
    paint.SetHeight(image_height);
    paint.Clear(WHITE);
    display_status_message(0, Font24, item, args...);
    epd.WaitUntilIdle();
    epd.SetFrameMemory(paint.GetImage(), 0, 0, paint.GetWidth(), paint.GetHeight());
    epd.DisplayFrame();
}

static int DrawFrameQRTextCode(const String &qr, int lines)
{
    Serial.println("Generating QR Frame");
    Serial.flush();
    QRCode frame_qrcode;
    uint8_t version{5};
    uint8_t qrcodeData[qrcode_getBufferSize(version)];
    if (qrcode_initText(&frame_qrcode, qrcodeData, version, 0, qr.c_str()) != 0)
    {
        return 0;
    }
    constexpr uint16_t blockSize{3};
    paint.SetHeight(frame_qrcode.size * blockSize);
    paint.SetWidth(frame_qrcode.size * blockSize);
    paint.Clear(WHITE);
    for (uint8_t x = 0; x < frame_qrcode.size; ++x)
    {
        for (uint8_t y = 0; y < frame_qrcode.size; ++y)
        {
            auto module{qrcode_getModule(&frame_qrcode, x, y)};
            uint16_t rect_x{x * blockSize};
            uint16_t rect_y{y * blockSize};
            paint.DrawFilledRectangle(rect_x, rect_y, rect_x + blockSize - 1, rect_y + blockSize - 1, module ? BLACK : WHITE);
        }
    }

    epd.SetFrameMemory(paint.GetImage(), (epd.width - paint.GetWidth()) / 2, Font24.Height + Font16.Height * lines, paint.GetWidth(), paint.GetHeight());

    return frame_qrcode.size * blockSize;
}

/**
 * @brief Display the Initialize message.
 *
 * This will be displayed by the WiFi manager's AP creation callback.
 */
static void display_initialize_message(WiFiManager *w)
{

    // Create a WiFi QR
    Serial.println("Displaying initialize message");
    String ssid{w->getConfigPortalSSID()};
    epd.LDirInit();
    epd.DisplayPartBaseWhiteImage();
    String qr_string{"WIFI:S:"};
    qr_string += ssid;
    qr_string += ";T:WPA;P:";
    qr_string += password;
    qr_string += ";H:;;";
    Serial.println(qr_string);

    paint.SetWidth(epd.width);
    paint.SetHeight(std::min(epd.height, epd.height - Font24.Height - 3*Font16.Height));
    paint.Clear(WHITE);

    display_status_message(0, Font24, "Setup WiFi",
        "Connect to",
        ssid.c_str(),
        password);
    epd.SetFrameMemory(paint.GetImage(), 0, 0, paint.GetWidth(), paint.GetHeight());
    int qr_height{ DrawFrameQRTextCode(qr_string, 4) };
    if (qr_height == 0)
    {
        Serial.println("QR code generation failure");
        display_status_message("Setup WiFi",
            "Connect to the",
            "WiFi network",
                ssid.c_str(),
            "password",
            password,
            "and configure your",
            "WiFi settings");
        return;
    }
    epd.DisplayFrame();
}

/* Entry point ----------------------------------------------------------------*/
void setup()
{
    Serial.begin(115200);
    Serial.println("Image Display ");
    epd.LDirInit();
    epd.Clear();
    display_status_message("Initializing");

    WiFiManager wifiManager;

    wifiManager.setAPCallback(display_initialize_message);

    // Generate a random SSID and password.
    static char ssid[64];
    strcpy(ssid, "ImageLoad");
    for (int i = 0; i < 3; ++i)
    {
#ifdef ESP8266
        ssid[i + 9] = ESP8266TrueRandom.random('0','9');
#elif defined(ESP32)
        ssid[i + 9] = esp_random() % 10 + '0';
#endif
    }

    ssid[3 + 9] = '\0';
    for (int i = 0; i < 8; ++i)
    {
#ifdef ESP8266
        password[i] = ESP8266TrueRandom.random('0', '9');
#elif defined(ESP32)
        password[i] = esp_random() % 10 + '0';
#endif
    }
    password[8] = '\0';

    Serial.println("Going to autoconnect, no-connnect AP SSID=" + String(ssid) + " password=" + String(password));
    Serial.flush();
    //fetches ssid and pass from eeprom and tries to connect
    //if it does not connect it starts an access point with the specified name
    //here  "AutoConnectAP"
    //and goes into a blocking loop awaiting configuration
    wifiManager.autoConnect(ssid, password);

    LittleFS.begin();

    // Set up the web server.
    server.onNotFound([](AsyncWebServerRequest *request)
    {
        request->send(404, "text/plain", "Not found");
    });

    server.onFileUpload(handleUpload);

    server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
        request->send_P(200, "text/html", index_html, processor);
    });

    server.on("/listfiles", HTTP_GET, [](AsyncWebServerRequest * request) {
        request->send(200, "text/html", listFiles(true));
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest * request) {
        auto param{ request->getParam("file")};
        if (param != nullptr)
        {
            LittleFS.remove(param->value());
        }
        request->send(200, "text/plain", "Deleted File: " + param->value());
    });
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest * request) {
        auto param{ request->getParam("file")};
        if (param != nullptr)
        {
            request->send(LittleFS, param->value(), String(), true);
        }
        else
        {
            request->send(405, "Missing parameter");
        }
    });

    server.on("/state", HTTP_GET, [](AsyncWebServerRequest * request) {
        FSInfo64 info;
        LittleFS.info64(info);
        String state{"{"};
        state += "\"currentImage\":\"" + currentImage + "\"," +
            "\"epdstate\":\"" + epdState + "\"," +
            "\"freestorage\":\"" + humanReadableSize((info.totalBytes - info.usedBytes)) + "\"," +
            "\"usedstorage\":\"" + humanReadableSize((info.usedBytes)) + "\"," +
            "\"totaltorage\":\"" + humanReadableSize((info.totalBytes)) + "\"}";
        request->send(200, "application/json", state);
    });
    server.on("/display", HTTP_GET, [](AsyncWebServerRequest * request) {
        auto param{ request->getParam("file")};
        if (param != nullptr)
        {
            String name{param->value()};
            if (LittleFS.exists(name))
            {
                currentImage = name;
                epdState = "displaying image";
                display_image(&name);
                request->send(200, "text/plain", "Loaded image file: " + name);
            }
            else
            {
                request->send(404, "text/plain", "Image file " + name + " not found");
            }
        }
    });
    server.on("/sleep", HTTP_GET, [](AsyncWebServerRequest * request) {
        epd.Sleep();
        epdState = "sleeping";
        request->send(200, "OK");
    });
    server.on("/clear", HTTP_GET, [](AsyncWebServerRequest * request) {
        currentImage = "<none>";
        epdState = "cleared";
        epd.HDirInit();
        epd.Clear();
        request->send(200, "OK");
    });

    server.on("/qr", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        auto version{request->getParam("version", true)};
        auto ecc{request->getParam("ecc", true)};
        auto text{request->getParam("text", true)};
        if (version == nullptr || ecc == nullptr || text== nullptr)
        {
            request->send(405, "Missing parameters");
            return;
        }

        qr_code_version = std::atoi(version->value().c_str());
        qr_code_ecc = std::atoi(ecc->value().c_str());
        qr_code_text = text->value();
        qr_code_scale = request->getParam("scale", true) != nullptr;

        static Ticker ticker;
#ifdef ESP8266
        ticker.once_ms_scheduled(500, display_qr_code);
#else
        ticker.once_ms(500, display_qr_code);
#endif

        request->redirect("/");

    });
    server.begin();

    String myIp{ WiFi.localIP().toString() };

    epd.LDirInit();
    epd.DisplayPartBaseWhiteImage();

    paint.SetWidth(epd.width);
    paint.SetHeight(Font24.Height + 2 * Font16.Height);
    paint.Clear(WHITE);
    display_status_message(0, Font24, "Ready",
        "Connect to http://",
        myIp.c_str());
    epd.SetFrameMemory(paint.GetImage(), 0, 0, paint.GetWidth(), paint.GetHeight());
    DrawFrameQRTextCode("http://"+ myIp, 3);
    epd.DisplayFrame();
}

/* The main loop -------------------------------------------------------------*/
void loop()
{
}

/**
 * @brief
 *
 * @param row
 * @param col
 * @param lcdidx
 * @param lcdbuffer
 */
static inline void pushColours(int row, int col, int lcdidx, uint16_t lcdbuffer[])
{
    bool anyBlack = false;
    for (int idx = 0; idx < lcdidx; ++idx)
    {
        anyBlack = anyBlack | lcdbuffer[idx] != 0;
        paint.DrawPixel(row, col + idx, lcdbuffer[idx] != 0 ? WHITE : BLACK);
    }
    yield();
}

static constexpr int BUFFPIXEL{20};


/**
 * @brief Read an arbitrary binary value from a file.
 *
 * Intended for POD types, such as int16 or int32.
 *
 * @tparam T Container type.
 * @param f  File to read frm.
 * @return Data read.
 */
template<typename T>
T read(File &f)
{
    T result;
    f.read(reinterpret_cast<uint8_t *>(&result), sizeof(result));
    return result;
}

/**
 * @brief Read 16 bytes, in platform order.
 *
 * @param f File to read from.
 * @return 16 bytes, platform order.
 */
static inline uint16_t read16(File& f)
{
    return read<uint16_t>(f);
}

/**
 * @brief Read 32 bytes, in platform order.
 *
 * @param f File to read from.
 * @return 32 bytes, platform order.
 */
static inline uint32_t read32(File& f)
{
    return read<uint32_t>(f);
}

/**
 * @brief Read a BMP file and push it to the paint object.
 *
 * This is adapted with very minor changes from the Adafruit source.
 *
 * @param filename File to read from.
 * @param x        Offset in paint to store the image.
 * @param y        Offset in paint to store the image.
 */
void bmpDraw(const char *filename, int x, int y)
{

  File     bmpFile;
  int      bmpWidth, bmpHeight;   // W+H in pixels
  uint8_t  bmpDepth;              // Bit depth (currently must be 24)
  uint32_t bmpImageoffset;        // Start of image data in file
  uint32_t rowSize;               // Not always = bmpWidth; may have padding
  uint8_t  sdbuffer[3*BUFFPIXEL]; // pixel in buffer (R+G+B per pixel)
  uint16_t lcdbuffer[BUFFPIXEL];  // pixel out buffer (16-bit per pixel)
  uint8_t  buffidx = sizeof(sdbuffer); // Current position in sdbuffer
  boolean  goodBmp = false;       // Set to true on valid header parse
  boolean  flip    = true;        // BMP is stored bottom-to-top
  int      w, h, row, col;
  uint8_t  r, g, b;
  uint32_t pos = 0, startTime = millis();
  uint8_t  lcdidx = 0;
  boolean  first = true;

  if((x >= paint.GetWidth()) || (y >= paint.GetHeight())) return;

  Serial.println();
  Serial.print(F("Loading image '"));
  Serial.print(filename);
  Serial.println('\'');
  // Open requested file on SD card
  if (!(bmpFile = LittleFS.open(filename, "r"))) {
    Serial.println(F("File not found"));
    return;
  }

  // Parse BMP header
  if(read16(bmpFile) == 0x4D42) { // BMP signature
    Serial.println(F("File size: ")); Serial.println(read32(bmpFile));
    (void)read32(bmpFile); // Read & ignore creator bytes
    bmpImageoffset = read32(bmpFile); // Start of image data
    Serial.print(F("Image Offset: ")); Serial.println(bmpImageoffset, DEC);
    // Read DIB header
    Serial.print(F("Header size: ")); Serial.println(read32(bmpFile));
    bmpWidth  = read32(bmpFile);
    bmpHeight = read32(bmpFile);
    if(read16(bmpFile) == 1) { // # planes -- must be '1'
      bmpDepth = read16(bmpFile); // bits per pixel
      Serial.print(F("Bit Depth: ")); Serial.println(bmpDepth);
      if((bmpDepth == 24) && (read32(bmpFile) == 0)) { // 0 = uncompressed

        goodBmp = true; // Supported BMP format -- proceed!
        Serial.print(F("Image size: "));
        Serial.print(bmpWidth);
        Serial.print('x');
        Serial.println(bmpHeight);

        // BMP rows are padded (if needed) to 4-byte boundary
        rowSize = (bmpWidth * 3 + 3) & ~3;

        // If bmpHeight is negative, image is in top-down order.
        // This is not canon but has been observed in the wild.
        if(bmpHeight < 0) {
          bmpHeight = -bmpHeight;
          flip      = false;
        }

        // Crop area to be loaded
        w = bmpWidth;
        h = bmpHeight;
        if((x+w-1) >= paint.GetWidth())  w = paint.GetWidth()  - x;
        if((y+h-1) >= paint.GetHeight()) h = paint.GetHeight() - y;

        // Set TFT address window to clipped image bounds
        // tft.setAddrWindow(x, y, x+w-1, y+h-1);

        for (row=0; row<h; row++) { // For each scanline...
          // Seek to start of scan line.  It might seem labor-
          // intensive to be doing this on every line, but this
          // method covers a lot of gritty details like cropping
          // and scanline padding.  Also, the seek only takes
          // place if the file position actually needs to change
          // (avoids a lot of cluster math in SD library).
          if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
            pos = bmpImageoffset + (bmpHeight - 1 - row) * rowSize;
          else     // Bitmap is stored top-to-bottom
            pos = bmpImageoffset + row * rowSize;
          if(bmpFile.position() != pos) { // Need seek?
            bmpFile.seek(pos);
            buffidx = sizeof(sdbuffer); // Force buffer reload
          }

          for (col=0; col<w; col++) { // For each column...
            // Time to read more pixel data?
            if (buffidx >= sizeof(sdbuffer)) { // Indeed
              // Push LCD buffer to the display first
              if(lcdidx > 0) {
                pushColours(row, col - lcdidx, lcdidx, lcdbuffer);
                // tft.pushColors(lcdbuffer, lcdidx, first);
                lcdidx = 0;
                first  = false;
              }
              bmpFile.read(sdbuffer, sizeof(sdbuffer));
              buffidx = 0; // Set index to beginning
            }

            // Convert pixel from BMP to TFT format
            b = sdbuffer[buffidx++];
            g = sdbuffer[buffidx++];
            r = sdbuffer[buffidx++];
            lcdbuffer[lcdidx++] = (r << 24) | (g << 8) || b; // tft.color565(r,g,b);
          } // end pixel
        // Write any remaining data to LCD
            if(lcdidx > 0) {
                pushColours(row, col - lcdidx, lcdidx, lcdbuffer);
                lcdidx = 0;
            // tft.pushColors(lcdbuffer, lcdidx, first);
            }
        } // end scanline
        Serial.print(F("Loaded in "));
        Serial.print(millis() - startTime);
        Serial.println(" ms");
      } // end goodBmp
    }
  }

  bmpFile.close();
  if(!goodBmp) Serial.println(F("BMP format not recognized."));
}

// The ESP8266 has insufficient program memory to support
// reading a PNG file.
#ifndef ESP8266
File myfile;
PNG png;

void * myOpen(const char *filename, int32_t *size) {
  Serial.printf("Attempting to open %s\n", filename);
  myfile = LittleFS.open(filename, "r");
  *size = myfile.size();
  return &myfile;
}
void myClose(void *handle) {
  if (myfile) myfile.close();
}
int32_t myRead(myfile *handle, uint8_t *buffer, int32_t length) {
  if (!myfile) return 0;
  return myfile.read(buffer, length);
}
int32_t mySeek(myfile *handle, int32_t position) {
  if (!myfile) return 0;
  return myfile.seek(position);
}

void PNGDraw(PNGDRAW *pDraw) {
    uint16_t usPixels[320];

    png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int i = 0; i < pDraw->iWidth && i < paint.GetWidth(); ++i)
    {
        paint.DrawPixel(i, pDraw->y, usPixels[i] !=  0 ? BLACK : WHITE);
    }
}
#endif

static void display_image(const String *filename)
{
    if (filename->endsWith(".png"))
    {
#ifndef ESP8266
        int rc = png.open(filename->c_str(), myOpen, myClose, myRead, mySeek, PNGDraw);
         if (rc == PNG_SUCCESS) {
            Serial.printf("image specs: (%d x %d), %d bpp, pixel type: %d\n", png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType());
            epdState = "active";
            epd.LDirInit();
            epd.Clear();
            paint.SetWidth(image_width);
            paint.SetHeight(image_height);
            paint.Clear(WHITE);
            rc = png.decode(NULL, 0);
            png.close();
            epd.WaitUntilIdle();
            // Because it's full size, this is a short-cut.
            epd.DisplayPart(paint.GetImage());
        }
#endif
    }
    else if (filename->endsWith(".bmp") || filename->endsWith(".BMP"))
    {
        epdState = "active";
        epd.LDirInit();
        epd.Clear();
        Serial.println("Attempting to display image");
        paint.SetWidth(image_width);
        paint.SetHeight(image_height);
        paint.Clear(WHITE);
        bmpDraw(filename->c_str(), 0, 0);
        // Because it's full size, this is a short-cut.
        epd.DisplayPart(paint.GetImage());
        currentImage = *filename;
    }

}
static void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    Serial.println(logmessage);

    if (!index) {
        logmessage = "Upload Start: " + String(filename);
        // open the file on first call and store the file handle in the request object
        request->_tempFile = LittleFS.open("/" + filename, "w");
        Serial.println(logmessage);
    }

    if (len) {
        // stream the incoming chunk to the opened file
        request->_tempFile.write(data, len);
        logmessage = "Writing file: " + String(filename) + " index=" + String(index) + " len=" + String(len);
        Serial.println(logmessage);
    }

    if (final) {
        logmessage = "Upload Complete: " + String(filename) + ",size: " + String(index + len);
        // close the file handle as the upload is now done
        request->_tempFile.close();
        Serial.println(logmessage);
        static Ticker loadTicker;
        currentImage = filename;
        epdState = "displaying image";
#ifdef ESP8266
        loadTicker.once_ms_scheduled(2000, [filename] ()
        {
            yield();
            display_image(&filename);
        });
#else
        static String loadFileName;
        loadFileName = filename;
        loadTicker.once(2.0f, display_image, &loadFileName);
#endif
        request->redirect("/");
    }
}

static String listFiles(bool ishtml)
{
    String returnText = "";
    Serial.println("Listing files stored on LittleFS");
    auto files_root = LittleFS.openDir("/");
    if (ishtml) {
        returnText += "<table><tr><th align='left'>Name</th><th align='left'>Size</th></tr>";
    }
    while (files_root.next()) {
        if (ishtml) {
            returnText += "<tr align='left'><td>" + files_root.fileName() + "</td><td>" + humanReadableSize(files_root.fileSize()) + "</td>";
            if (files_root.fileName().endsWith(".bmp") || files_root.fileName().endsWith(".BMP"))
            {
                returnText += "<td><a href=\"/display?file=" + files_root.fileName() + "\">Display</a></td><td><image src=\"/download?file=" + files_root.fileName() + "\"></td>";
            }
            else
            {
                returnText += "<td></td><td></td>";
            }
            returnText += "<td><a href=\"/download?file=" + files_root.fileName() + "\" target=\"_blank\">Download</a><td><button onclick=\"deleteButton(\'" + files_root.fileName() + "\', \'delete\')\">Delete</button></tr>";
        } else {
            returnText += "File: " + files_root.fileName() + "\n";
        }
    }
    if (ishtml) {
        returnText += "</table>";
    }
    return returnText;
}

static String processor(const String& var)
{
    FSInfo64 info;
    LittleFS.info64(info);
    if (var == "FILELIST") {
        return listFiles(true);
    }
    if (var == "FREESPIFFS") {
        return humanReadableSize((info.totalBytes - info.usedBytes));
    }

    if (var == "USEDSPIFFS") {
        return humanReadableSize(info.usedBytes);
    }

    if (var == "TOTALSPIFFS") {
        return humanReadableSize(info.totalBytes);
    }

    if (var == "EPDSTATE") {
        return epdState;
    }

    if (var == "CURRENTIMAGE") {
        return currentImage;
    }

    return String();
}

// Make size of files human readable
// source: https://github.com/CelliesProjects/minimalUploadAuthESP32
static String humanReadableSize(const size_t bytes)
{
    if (bytes < 1024) return String(bytes) + " B";
    else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
    else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
    else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
}

static void display_qr_code()
{
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(qr_code_version)];
    if (qrcode_initText(&qrcode, qrcodeData, qr_code_version, qr_code_ecc, qr_code_text.c_str()) != 0)
    {
        Serial.println("QR code generation failure");
        return;
    }
    yield();
    Serial.println("Code generated");
    Serial.flush();


    if (qrcode.size > epd.height || qrcode.size > epd.width)
    {
        Serial.println("QR code too large for your display, which is " + String(epd.width) + "x" + String(epd.height));
        return;
    }

    uint16_t blockSize{qr_code_scale ?  static_cast<int>(std::min(epd.height, epd.width)) / qrcode.size : 1};
    Serial.println("Generated, filling display QR=" + String(qrcode.size) + " pixels with blockSize = " + String(blockSize));
    Serial.flush();
    epd.HDirInit();
    epd.Clear();

    paint.SetHeight(epd.width);
    paint.SetWidth(epd.height);
    paint.Clear(WHITE);
    uint16_t display_x = (paint.GetWidth() - qrcode.size * blockSize) / 2;
    uint16_t display_y = (paint.GetHeight() - qrcode.size * blockSize) / 2;
    for (uint8_t x = 0; x < qrcode.size; ++x)
    {
        for (uint8_t y = 0; y < qrcode.size; ++y)
        {
            auto module{qrcode_getModule(&qrcode, x, y)};
            uint16_t rect_x{display_x + x * blockSize};
            uint16_t rect_y{display_y + y * blockSize};
            paint.DrawFilledRectangle(rect_x, rect_y, rect_x + blockSize - 1, rect_y + blockSize - 1, module ? BLACK : WHITE);
        }
        yield();
    }
    // Center paint.
    epd.WaitUntilIdle();
    epd.DisplayPart(paint.GetImage());
    snapshot(paint);

    epdState = "showing generated QR";
    currentImage = "generated QR";
}

template<typename T, size_t N>
static inline size_t countof(const T (&)[N])
{
    return N;
}

static void snapshot(Paint &snapshotPaint)
{
    static const char *outfile{ "/generated-qr-code.bmp" };
    fs::File image = LittleFS.open(outfile, "w");
    if (image)
    {
        int convert_width{0};
        int convert_height{0};
        convert_width = snapshotPaint.GetWidth();
        convert_height = snapshotPaint.GetHeight();
        auto imageData{snapshotPaint.GetImage()};
        constexpr int snapshot_row_height{ 1 };

        uint8_t bmpfileheader[14] = {'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0};
        uint8_t bmpinfoheader[40] = {40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0};
        int filesize = countof(bmpfileheader) + countof(bmpinfoheader) + 3*convert_width*convert_height;

        bmpfileheader[ 2] = static_cast<uint8_t>(filesize    );
        bmpfileheader[ 3] = static_cast<uint8_t>(filesize>> 8);
        bmpfileheader[ 4] = static_cast<uint8_t>(filesize>>16);
        bmpfileheader[ 5] = static_cast<uint8_t>(filesize>>24);

        bmpinfoheader[ 4] = static_cast<uint8_t>(       convert_width    );
        bmpinfoheader[ 5] = static_cast<uint8_t>(       convert_width>> 8);
        bmpinfoheader[ 6] = static_cast<uint8_t>(       convert_width>>16);
        bmpinfoheader[ 7] = static_cast<uint8_t>(       convert_width>>24);
        bmpinfoheader[ 8] = static_cast<uint8_t>(       convert_height    );
        bmpinfoheader[ 9] = static_cast<uint8_t>(       convert_height>> 8);
        bmpinfoheader[10] = static_cast<uint8_t>(       convert_height>>16);
        bmpinfoheader[11] = static_cast<uint8_t>(       convert_height>>24);


        image.write(bmpfileheader, countof(bmpfileheader));
        image.write(bmpinfoheader, countof(bmpinfoheader));
        // Bottom to top to account for the BMP format.
        uint8_t rect[convert_width * snapshot_row_height * 3];
        for (int row = convert_height - 1; row >= 0; --row)
        {
            for (int col = 0; col < convert_width; ++col)
            {
                uint8_t rgb{ (imageData[(row * convert_width + col) / 8] & (0x80 >> (col % 8))) != 0 ? 0xFFu : 0 };
                // Set bitmap colour to white or black.
                rect[col * 3] = rgb;
                rect[col * 3 + 1] = rgb;
                rect[col * 3 + 2] = rgb;
            }
            image.write(rect, sizeof(rect));
            static const uint8_t padding[3] { 0, 0, 0 };
            if (convert_width % 4 != 0)
            {
                image.write(padding, 4 - (convert_width % 4));
            }
        }
        image.close();
    }
}