// =============================================================================
// Bitcoin Ticker firmware for M5Stick (ESP32)
//
// - Connects to WiFi (WPA2-Personal or WPA2-Enterprise depending on build)
// - Fetches the current Bitcoin price from CoinGecko every 10 seconds
// - Checks GitHub for newer firmware releases every 60 seconds
// - Performs an OTA (over-the-air) firmware update when a newer version exists
// =============================================================================

#include <M5Unified.h>
#include "WiFi.h"
#include "esp_wifi.h"
// Optional WPA2-Enterprise support: only compiled in if the header is available
#if __has_include("esp_wpa2.h")
#include "esp_wpa2.h"
#define HAS_WPA2_ENTERPRISE 1
#else
#define HAS_WPA2_ENTERPRISE 0
#endif
#include "WiFiClient.h"
#include "WiFiClientSecure.h"
#include "HTTPClient.h"
#include "Update.h"

#define FIRMWARE_VERSION "1.0.0"

// WiFi and GitHub credentials handling:
// - GitHub Actions: Uses temporary header file (include/wifi_config.h) with secrets injected at build time
// - Local development: Uses environment variables via platformio.ini build_flags

#if __has_include("wifi_config.h")
  // CI/CD: Include temporary header with secrets from GitHub Actions
  #include "wifi_config.h"
#else
  // Local development: Use environment variables passed via build_flags

  #ifndef GITHUB_TOKEN
  #define GITHUB_TOKEN ""
  #endif

  #ifndef D_WIFI_USER
  #define D_WIFI_USER ""
  #endif

  #ifndef D_WIFI_SSID
  #define D_WIFI_SSID ""
  #endif

  #ifndef D_WIFI_PASS
  #define D_WIFI_PASS ""
  #endif
  
#endif

// For WPA2-Enterprise networks, the identity is typically the same as the username
#define D_WIFI_IDENTITY D_WIFI_USER

// Endpoints and identifiers used by the firmware
#define BITCOIN_PRICE_ENDPOINT "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
#define GITHUB_API_ENDPOINT "https://api.github.com/repos/giacomobenedetti/test-it/releases/latest"
#define GITHUB_REPO "giacomobenedetti/test-it"


// ---- Global state -----------------------------------------------------------
// Bitcoin price tracking
static uint32_t g_bitcoinPriceUsd = 0;
static uint32_t g_lastBitcoinCheckMs = 0;
static const uint32_t BITCOIN_CHECK_INTERVAL_MS = 10000;   // poll price every 10 s

// Firmware update tracking
static uint32_t g_lastFirmwareCheckMs = 0;
static const uint32_t FIRMWARE_CHECK_INTERVAL_MS = 60000;  // poll GitHub every 60 s
static char g_latestFirmwareVersion[32] = FIRMWARE_VERSION;
static String g_firmwareDownloadUrl = "";

// Status message shown on the device display
static String g_lastStatusMsg = "";
static uint32_t g_lastStatusUpdateMs = 0;

// Updates the on-screen status line and mirrors it to the serial log
static void setStatus(const char* msg) {
	g_lastStatusMsg = msg;
	g_lastStatusUpdateMs = millis();
	Serial.println(msg);
	// Update status line (line 4)
	M5.Lcd.setCursor(36, 40);
	M5.Lcd.setTextSize(1);
	M5.Lcd.printf("%-10s", msg);
	M5.Lcd.setTextSize(1);
}

// Forward declaration: defined further down, called from checkFirmwareUpdate()
static void performFirmwareUpdate();


// -----------------------------------------------------------------------------
// fetchBitcoinPrice
// Calls the CoinGecko price endpoint, parses the BTC/USD value from the JSON
// response, and updates both the global price and the on-screen display.
// -----------------------------------------------------------------------------
static void fetchBitcoinPrice() {
	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("Bitcoin fetch: WiFi not connected");
		return;
	}

	// Use an insecure TLS client: skips certificate validation.
	// Acceptable for a demo; in production you'd pin the CA or use a cert bundle.
	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;
	http.setTimeout(5000);

	if (!http.begin(client, BITCOIN_PRICE_ENDPOINT)) {
		Serial.println("Bitcoin fetch: Failed to begin HTTP");
		return;
	}

	int httpCode = http.GET();

	if (httpCode != HTTP_CODE_OK) {
		Serial.printf("Bitcoin fetch: HTTP error %d\n", httpCode);
		http.end();
		return;
	}

	String payload = http.getString();
	http.end();

	Serial.printf("Bitcoin response: %s\n", payload.c_str());

	// Manual JSON parsing: locate "usd": and read the number that follows.
	// A real JSON library would be safer, but this keeps the firmware small.
	int pos = payload.indexOf("\"usd\":");
	if (pos != -1) {
		pos += 6;
		int endPos = payload.indexOf('}', pos);
		if (endPos == -1) endPos = payload.indexOf(',', pos);
		if (endPos == -1) endPos = payload.length();

		String priceStr = payload.substring(pos, endPos);
		priceStr.trim();

		float price = strtof(priceStr.c_str(), nullptr);
		g_bitcoinPriceUsd = (uint32_t)price;
		Serial.printf("Bitcoin price parsed: $%lu USD\n", (unsigned long)g_bitcoinPriceUsd);
	} else {
		Serial.println("Bitcoin: Could not find \"usd\" in response");
	}
	// Update display
	M5.Lcd.setCursor(36, 32);
	M5.Lcd.printf("%-7lu", (unsigned long)g_bitcoinPriceUsd);
}


// -----------------------------------------------------------------------------
// checkFirmwareUpdate
// Queries the GitHub Releases API for the latest release of this project.
// If the latest tag differs from FIRMWARE_VERSION, locates the firmware.bin
// asset URL and triggers an OTA update.
// -----------------------------------------------------------------------------
static void checkFirmwareUpdate() {
	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("Firmware check: WiFi not connected");
		return;
	}

	Serial.println("=== Starting GitHub firmware check ===");

	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;
	http.setTimeout(5000);
	// GitHub's API requires a User-Agent header on every request
	http.addHeader("User-Agent", "esp32-iot-device");
	// Authenticated requests have a higher rate limit than anonymous ones
	http.addHeader("Authorization", "token " + String(GITHUB_TOKEN));
	http.addHeader("Accept", "application/vnd.github.v3+json");

	if (!http.begin(client, GITHUB_API_ENDPOINT)) {
		setStatus("API conn fail");
		Serial.println("❌ Failed to connect to GitHub API");
		return;
	}

	int httpCode = http.GET();
	Serial.printf("GitHub API HTTP response: %d\n", httpCode);

	// Log all response headers
	String allHeaders = http.header("Server");
	Serial.printf("Server: %s\n", allHeaders.c_str());

	// 403 from GitHub usually means we hit the API rate limit
	if (httpCode == 403) {
		setStatus("Rate limit");
		Serial.println("❌ GitHub API rate limit exceeded. Waiting...");
		http.end();
		g_lastFirmwareCheckMs = millis(); // Reset timer
		return;
	}

	if (httpCode != HTTP_CODE_OK) {
		setStatus("API err");
		Serial.printf("❌ GitHub API error %d\n", httpCode);
		// Print response body for debugging
		String response = http.getString();
		Serial.printf("Response: %s\n", response.c_str());
		http.end();
		return;
	}

	String payload = http.getString();
	http.end();

	Serial.printf("Response length: %d bytes\n", payload.length());
	Serial.printf("First 500 chars: %.500s\n", payload.c_str());

	// Extract the release tag name (e.g. "v1.0.1") from the JSON response
	int tagPos = payload.indexOf("\"tag_name\":");
	Serial.printf("tag_name position: %d\n", tagPos);

	if (tagPos == -1) {
		setStatus("No tag found");
		Serial.println("❌ No tag_name found in response");
		return;
	}

	tagPos += 11;
	int tagEnd = payload.indexOf('"', tagPos + 1);
	String latestTag = payload.substring(tagPos + 1, tagEnd);
	latestTag.trim();
	Serial.printf("✓ Latest tag: %s\n", latestTag.c_str());

	// Remove 'v' prefix for comparison
	String latestVersion = latestTag;
	if (latestVersion.startsWith("v")) {
		latestVersion = latestVersion.substring(1);
	}

	Serial.printf("Current FW: %s | Latest: %s\n", FIRMWARE_VERSION, latestVersion.c_str());

	// Extract download URL for firmware.bin
	int assetsPos = payload.indexOf("\"assets\":");
	Serial.printf("assets position: %d\n", assetsPos);

	if (assetsPos == -1) {
		setStatus("No assets");
		Serial.println("❌ No assets found");
		return;
	}

	// A release can have multiple assets — walk the list and pick firmware.bin
	int searchStart = assetsPos;
	int urlPos = -1;

	for (int i = 0; i < 5; i++) {
		int tempUrlPos = payload.indexOf("\"browser_download_url\":\"", searchStart);
		if (tempUrlPos == -1) break;

		int urlEnd = payload.indexOf('"', tempUrlPos + 24);
		String testUrl = payload.substring(tempUrlPos + 24, urlEnd);
		Serial.printf("Found URL %d: %s\n", i, testUrl.c_str());

		if (testUrl.indexOf("firmware.bin") != -1) {
			urlPos = tempUrlPos + 24;
			searchStart = urlEnd + 1;
			break;
		}
		searchStart = urlEnd + 1;
	}

	if (urlPos == -1) {
		setStatus("No firmware.bin");
		Serial.println("❌ firmware.bin not found in assets");
		return;
	}

	int urlEnd = payload.indexOf('"', urlPos);
	g_firmwareDownloadUrl = payload.substring(urlPos, urlEnd);
	Serial.printf("✓ Download URL: %s\n", g_firmwareDownloadUrl.c_str());

	// Only update if the latest tag is different from what's currently running
	if (latestVersion != FIRMWARE_VERSION) {
		setStatus("Updating...");
		Serial.printf("✓ New firmware available: %s > %s\n",
			latestVersion.c_str(), FIRMWARE_VERSION);
		strncpy(g_latestFirmwareVersion, latestVersion.c_str(), sizeof(g_latestFirmwareVersion) - 1);
		performFirmwareUpdate();
	} else {
		setStatus("Up to date");
		Serial.println("✓ Already on latest version");
	}
}

// -----------------------------------------------------------------------------
// performFirmwareUpdate
// Downloads firmware.bin from the URL discovered by checkFirmwareUpdate(),
// streams it into the ESP32 Update partition, and reboots on success.
// -----------------------------------------------------------------------------
static void performFirmwareUpdate() {
	Serial.println("=== Starting firmware update ===");
	setStatus("DL firmware...");

	if (g_firmwareDownloadUrl.length() == 0) {
		setStatus("No URL");
		Serial.println("❌ No firmware download URL available");
		return;
	}

	Serial.printf("Download URL: %s\n", g_firmwareDownloadUrl.c_str());

	WiFiClientSecure client;
	client.setInsecure();
	HTTPClient http;
	http.setTimeout(30000);  // 30 second timeout for firmware download
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // Follow 302 redirects

	if (!http.begin(client, g_firmwareDownloadUrl)) {
		Serial.println("❌ Failed to connect to download URL");
		setStatus("DL failed");
		return;
	}

	int httpCode = http.GET();
	Serial.printf("Download HTTP response: %d\n", httpCode);

	if (httpCode != HTTP_CODE_OK) {
		Serial.printf("❌ Download failed: HTTP %d\n", httpCode);
		http.end();
		return;
	}

	// We need the Content-Length up front so the OTA partition can be sized
	int contentLength = http.getSize();
	Serial.printf("Content-Length: %d bytes\n", contentLength);

	if (contentLength <= 0) {
		Serial.println("❌ Invalid content length");
		http.end();
		return;
	}

	Serial.printf("✓ Starting OTA write (%d bytes)...\n", contentLength);

	// Update.begin() reserves space in the inactive OTA partition
	if (!Update.begin(contentLength)) {
		Serial.println("❌ Not enough space to begin OTA");
		http.end();
		setStatus("OTA no space");
		return;
	}

	Serial.println("✓ OTA buffer ready, starting stream read...");
	setStatus("Streaming...");

	WiFiClient * stream = http.getStreamPtr();

	if (stream == nullptr) {
		Serial.println("❌ Failed to get stream pointer");
		Update.abort();
		http.end();
		setStatus("Stream err");
		return;
	}

	// writeStream() reads from the HTTP stream and writes directly to flash
	size_t written = Update.writeStream(*stream);
	Serial.printf("Stream write complete: %zu bytes\n", written);

	if (written != contentLength) {
		Serial.printf("❌ Written only %zu / %d bytes\n", written, contentLength);
		Update.abort();
		http.end();
		setStatus("Write ERR");
		return;
	}

	Serial.printf("✓ Wrote %zu bytes\n", written);

	// Update.end() finalizes the new partition; isFinished() confirms validity
	if (!Update.end()) {
		Serial.printf("❌ OTA Failed: %s\n", Update.errorString());
		return;
	}

	if (!Update.isFinished()) {
		Serial.println("❌ OTA did not complete successfully");
		return;
	}

	Serial.println("✓✓✓ OTA Update completed successfully. Rebooting...");
	setStatus("Success! Rebooting");
	http.end();
	delay(1000);
	// After restart, the bootloader will boot from the freshly written partition
	ESP.restart();
}



// -----------------------------------------------------------------------------
// connectToWifi
// Initializes WiFi in station mode. Uses WPA2-Enterprise authentication
// if the ESP-IDF WPA2 enterprise headers are available, otherwise falls
// back to standard WPA2-Personal (SSID + password).
// -----------------------------------------------------------------------------
static void connectToWifi() {
	WiFi.disconnect(true);
	WiFi.mode(WIFI_STA);

#if HAS_WPA2_ENTERPRISE
	// WPA2-Enterprise: provide identity, username, and password separately
	esp_wifi_sta_wpa2_ent_set_identity(reinterpret_cast<const uint8_t *>(D_WIFI_IDENTITY),
										 strlen(D_WIFI_IDENTITY));
	esp_wifi_sta_wpa2_ent_set_username(reinterpret_cast<const uint8_t *>(D_WIFI_USER),
										 strlen(D_WIFI_USER));
	esp_wifi_sta_wpa2_ent_set_password(reinterpret_cast<const uint8_t *>(D_WIFI_PASS),
										 strlen(D_WIFI_PASS));

	esp_wifi_sta_wpa2_ent_enable();
	WiFi.begin(D_WIFI_SSID);
#else
	// WPA2-Personal: just SSID + pre-shared key
	WiFi.begin(D_WIFI_SSID, D_WIFI_PASS);
#endif
}

// -----------------------------------------------------------------------------
// setup
// Standard Arduino entry point. Initializes serial, the M5 display, WiFi,
// then performs an initial Bitcoin price fetch and firmware update check.
// -----------------------------------------------------------------------------
void setup() {
	Serial.begin(115200);
	delay(500);

	// Initialize the M5 board and the LCD
	M5.begin();
	M5.Lcd.setRotation(3);
	M5.Lcd.fillScreen(BLACK);
	M5.Lcd.setTextSize(1);
	M5.Lcd.setCursor(0, 0);

	// WiFi
	connectToWifi();
	Serial.print("connecting");
	delay(5000);

	// Block until we have an IP — without networking the rest of the device is useless
	while(WiFi.status() != WL_CONNECTED) {
		Serial.print(".");
		delay(500);
	}
	Serial.println();

	Serial.println("\nWiFi Connected");
	Serial.println(WiFi.localIP());

	// Display header
	M5.Lcd.printf("test-iot\n");
	M5.Lcd.printf("IP: %.16s\n", WiFi.localIP().toString().c_str());
	M5.Lcd.printf("FW: %s\n", FIRMWARE_VERSION);
	M5.Lcd.printf("BTC: -\n");
	M5.Lcd.printf("STS: Init\n");

	// Fetch data
	fetchBitcoinPrice();
	checkFirmwareUpdate();
}

// -----------------------------------------------------------------------------
// loop
// Main run loop. Polls the Bitcoin price and the GitHub release feed on
// independent timers and updates the corresponding display lines.
// -----------------------------------------------------------------------------
void loop() {
	M5.update();
	const uint32_t now = millis();

	// Update Bitcoin price (line 3, position 36)
	if ((now - g_lastBitcoinCheckMs) >= BITCOIN_CHECK_INTERVAL_MS) {
		g_lastBitcoinCheckMs = now;
		fetchBitcoinPrice();
		M5.Lcd.setCursor(36, 32);
		M5.Lcd.printf("%-7lu", (unsigned long)g_bitcoinPriceUsd);
	}

	// Update firmware check (line 4, position 36)
	if ((now - g_lastFirmwareCheckMs) >= FIRMWARE_CHECK_INTERVAL_MS) {
		g_lastFirmwareCheckMs = now;
		checkFirmwareUpdate();
		M5.Lcd.setCursor(36, 40);
		M5.Lcd.printf("%-8s", g_lastStatusMsg.c_str());
	}

	delay(10);
}