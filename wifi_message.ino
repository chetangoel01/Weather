#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include "epd_driver.h"
#include "firasans.h"
#include <map>
#include <vector>
#include "weather_icons.h"

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> tools -> PSRAM -> OPI !!!"
#endif

// WiFi and mDNS settings
const char *ssid = "Madina807-2G";
const char *password = "cfm@1234";
const char *host = "lilygo";

// OpenWeatherMap settings
#include "secrets.h"
const char* apiKey = SECRET_API_KEY;
const char* city = "London";
const char* countryCode = "UK";

WebServer server(80);
uint8_t *framebuffer;
unsigned long lastWeatherUpdate = 0;
const unsigned long weatherUpdateInterval = 1800000; // 30 minutes

// Display areas for different sections
const Rect_t currentWeatherArea = {
    .x = 50,
    .y = 50,
    .width = 860,
    .height = 150
};

const Rect_t currentTempArea = {
    .x = 50,
    .y = 50,
    .width = 300,
    .height = 200
};

const Rect_t nextDayTimesArea = {
    .x = 550,
    .y = 50,
    .width = 360,
    .height = 200
};

const Rect_t forecastArea = {
    .x = 50,
    .y = 300,
    .width = 860,
    .height = 240
};

// Helper function to extract time from dt_txt
String getTimeFromDtTxt(const char* dt_txt) {
    String timeStr = String(dt_txt).substring(11, 16);
    return timeStr;
}

// Helper function to extract date from dt_txt
String getDateFromDtTxt(const char* dt_txt) {
    String dateStr = String(dt_txt).substring(5, 10);
    return dateStr;
}

// Helper function to get day name from date
String getDayName(const char* dt_txt) {
    int year = String(dt_txt).substring(0, 4).toInt();
    int month = String(dt_txt).substring(5, 7).toInt();
    int day = String(dt_txt).substring(8, 10).toInt();
    
    if (month < 3) {
        month += 12;
        year--;
    }
    int k = year % 100;
    int j = year / 100;
    int h = (day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) - (2 * j)) % 7;
    h = (h + 5) % 7;
    
    const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return String(days[h]);
}

// Convert Kelvin to Celsius
float kelvinToCelsius(float kelvin) {
    return kelvin - 273.15;
}

void setup() {
    Serial.begin(115200);
    
    // Initialize the display
    epd_init();
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("Memory allocation failed!");
        while (1);
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    
    // Initialize WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected to WiFi. IP: %s\n", WiFi.localIP().toString().c_str());
    
    if (MDNS.begin(host)) {
        MDNS.addService("http", "tcp", 80);
    }
    
    // Initialize time
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    server.on("/refresh", HTTP_GET, handleRefresh);
    server.begin();
    
    // Initial display update
    epd_poweron();
    epd_clear();
    displayWeather();
    epd_poweroff();
}

void displayWeather() {
    String url = "http://api.openweathermap.org/data/2.5/forecast?q=";
    url += city;
    url += ",";
    url += countryCode;
    url += "&appid=";
    url += apiKey;
    
    HTTPClient http;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(16384);
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
            epd_clear();
            
            // Last updated text (smaller font)
            int cursor_x = 50;
            int cursor_y = 30;
            char lastUpdatedStr[64];
            time_t now = time(nullptr);
            strftime(lastUpdatedStr, sizeof(lastUpdatedStr), "Last updated: %H:%M", localtime(&now));
            writeln((GFXfont *)&FiraSans, lastUpdatedStr, &cursor_x, &cursor_y, NULL);
            
            // Current Temperature
            JsonObject current = doc["list"][0];
            float current_temp = kelvinToCelsius(current["main"]["temp"]);
            const char* current_weather = current["weather"][0]["main"].as<const char*>();
            
            cursor_x = currentTempArea.x;
            cursor_y = currentTempArea.y + 100;
            char currentTempStr[32];
            sprintf(currentTempStr, "%.1f°C", current_temp);
            writeln((GFXfont *)&FiraSans, currentTempStr, &cursor_x, &cursor_y, NULL);
            
            // Draw current weather icon
            const unsigned char* currentIcon;
            if (String(current_weather) == "Clear") {
                currentIcon = SUN_ICON;
            } else if (String(current_weather) == "Rain") {
                currentIcon = RAIN_ICON;
            } else if (String(current_weather) == "Snow") {
                currentIcon = SNOW_ICON;
            } else {
                currentIcon = CLOUD_ICON;
            }
            drawWeatherIcon(currentTempArea.x + 150, currentTempArea.y + 100, currentIcon);
            
            // Next Day Times (Top Right)
            float morning_temp = -999;
            float evening_temp = -999;
            
            JsonArray list = doc["list"].as<JsonArray>();
            if (!list.isNull()) {
                for (JsonObject item : list) {
                    String time = getTimeFromDtTxt(item["dt_txt"]);
                    String date = getDateFromDtTxt(item["dt_txt"]);
                    
                    if (date != getDateFromDtTxt(doc["list"][0]["dt_txt"])) { // If it's tomorrow
                        if (time == "08:00" || time == "09:00") {
                            morning_temp = kelvinToCelsius(item["main"]["temp"]);
                        }
                        if (time == "18:00") {
                            evening_temp = kelvinToCelsius(item["main"]["temp"]);
                        }
                    }
                }
            }
            
            cursor_x = nextDayTimesArea.x;
            cursor_y = nextDayTimesArea.y + FiraSans.advance_y;
            
            char timesTempStr[64];
            sprintf(timesTempStr, "Tomorrow:");
            writeln((GFXfont *)&FiraSans, timesTempStr, &cursor_x, &cursor_y, NULL);
            
            cursor_y += FiraSans.advance_y;
            cursor_x = nextDayTimesArea.x;
            sprintf(timesTempStr, "Morning: %.1f°C", morning_temp);
            writeln((GFXfont *)&FiraSans, timesTempStr, &cursor_x, &cursor_y, NULL);
            
            cursor_y += FiraSans.advance_y;
            cursor_x = nextDayTimesArea.x;
            sprintf(timesTempStr, "Evening: %.1f°C", evening_temp);
            writeln((GFXfont *)&FiraSans, timesTempStr, &cursor_x, &cursor_y, NULL);
            
            // 4-Day Forecast with updated layout
            std::map<String, std::pair<float, float>> dailyMinMax;
            std::map<String, String> dailyWeather;
            
            // Process all forecasts to find daily min/max
            if (!list.isNull()) {
                for (JsonObject item : list) {
                    String date = getDateFromDtTxt(item["dt_txt"]);
                    float temp = kelvinToCelsius(item["main"]["temp"]);
                    String weather = String(item["weather"][0]["main"].as<const char*>());
                    
                    if (dailyMinMax.find(date) == dailyMinMax.end()) {
                        dailyMinMax[date] = std::make_pair(temp, temp);
                        dailyWeather[date] = weather;
                    } else {
                        dailyMinMax[date].first = min(dailyMinMax[date].first, temp);
                        dailyMinMax[date].second = max(dailyMinMax[date].second, temp);
                    }
                }
            }
            
            // Display forecasts in horizontal layout
            int forecast_x_spacing = 200;
            int day_count = 0;
            
            for(auto& day : dailyMinMax) {
                if (day_count > 0 && day_count <= 4) {
                    int x_pos = forecastArea.x + (day_count - 1) * forecast_x_spacing;
                    
                    // Day name
                    cursor_x = x_pos + 60; // Center align
                    cursor_y = forecastArea.y;
                    String dayName = getDayName(day.first.c_str());
                    writeln((GFXfont *)&FiraSans, dayName.c_str(), &cursor_x, &cursor_y, NULL);
                    
                    // Weather icon
                    const unsigned char* dayIcon;
                    if (dailyWeather[day.first] == "Clear") {
                        dayIcon = SUN_ICON;
                    } else if (dailyWeather[day.first] == "Rain") {
                        dayIcon = RAIN_ICON;
                    } else if (dailyWeather[day.first] == "Snow") {
                        dayIcon = SNOW_ICON;
                    } else {
                        dayIcon = CLOUD_ICON;
                    }
                    drawWeatherIcon(x_pos + 60, forecastArea.y + 80, dayIcon);
                    
                    // Temperature below icon
                    cursor_x = x_pos + 20;
                    cursor_y = forecastArea.y + 160;
                    char tempStr[32];
                    sprintf(tempStr, "%.1f-%.1f°C", day.second.first, day.second.second);
                    writeln((GFXfont *)&FiraSans, tempStr, &cursor_x, &cursor_y, NULL);
                }
                day_count++;
            }
            
            // Update display
            epd_draw_grayscale_image(epd_full_screen(), framebuffer);
        }
    }
    
    http.end();
}

void handleRefresh() {
    epd_poweron();
    displayWeather();
    epd_poweroff();
    server.send(200, "text/plain", "Weather display refreshed");
}

void loop() {
    server.handleClient();
    
    // Update weather every 30 minutes
    if (millis() - lastWeatherUpdate >= weatherUpdateInterval) {
        epd_poweron();
        displayWeather();
        epd_poweroff();
        lastWeatherUpdate = millis();
    }
    
    delay(100);
}