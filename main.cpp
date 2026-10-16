// Calorie Tracker GUI: SDL3 + Dear ImGui + libcurl + nlohmann/json
// Data source: USDA FoodData Central (https://fdc.nal.usda.gov/api-guide)

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <map>

using json = nlohmann::json;

static const char* DATA_FILE = "tracker_data.json";

static std::string trimText(const std::string& s) {
    const char* ws = " \t\r\n";
    std::size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return "";
    std::size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}
static const std::vector<std::string> MEALS = {"Breakfast", "Lunch", "Dinner", "Snacks"};
static const char* DATA_TYPES[] = {"Foundation", "SR Legacy", "Survey (FNDDS)", "Branded", "All"};

// ======================= Data model =======================
struct Entry {
    long long fdcId = 0;
    std::string description;
    double grams = 0;
    double kcalPer100g = 0;
    double kcal() const { return grams / 100.0 * kcalPer100g; }
};

struct FoodHit {
    long long fdcId = 0;
    std::string description;
    std::string dataType;
    double kcalPer100g = -1;  // -1 = no energy value
};

using Log = std::map<std::string, std::map<std::string, std::vector<Entry>>>;

struct Settings {
    std::string apiKey;
    int goal = 2000;
};

// ======================= Dates =======================
static std::string formatTm(const std::tm& tm, const char* fmt) {
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

static std::string todayStr() {
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    return formatTm(tm, "%Y-%m-%d");
}

static std::string shiftDate(const std::string& d, int days) {
    int y = 0, m = 0, dd = 0;
    if (std::sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd) != 3) return todayStr();
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = dd + days;
    tm.tm_hour = 12;  // avoid DST edge cases
    std::mktime(&tm);
    return formatTm(tm, "%Y-%m-%d");
}

static std::string prettyDate(const std::string& d) {
    int y = 0, m = 0, dd = 0;
    if (std::sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd) != 3) return d;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = dd;
    tm.tm_hour = 12;
    std::mktime(&tm);
    return formatTm(tm, "%A, %d %B %Y");
}

// ======================= Persistence =======================
static void saveAll(const Log& log, const Settings& s) {
    json j;
    j["settings"] = {{"apiKey", s.apiKey}, {"goal", s.goal}};
    j["log"] = json::object();
    for (const auto& [date, meals] : log)
        for (const auto& [meal, entries] : meals)
            for (const auto& e : entries)
                j["log"][date][meal].push_back({{"fdcId", e.fdcId},
                                                {"description", e.description},
                                                {"grams", e.grams},
                                                {"kcalPer100g", e.kcalPer100g}});
    std::ofstream(DATA_FILE) << j.dump(2);
}

static void loadAll(Log& log, Settings& s) {
    std::ifstream in(DATA_FILE);
    if (!in) return;
    try {
        json j = json::parse(in);
        const json* logJ = &j;
        if (j.contains("settings")) {
            s.apiKey = j["settings"].value("apiKey", "");
            s.goal = j["settings"].value("goal", 2000);
        }
        if (j.contains("log")) logJ = &j["log"];
        else if (j.contains("settings")) return;
        for (auto& [date, meals] : logJ->items())
            for (auto& [meal, arr] : meals.items())
                for (auto& o : arr) {
                    Entry e;
                    e.fdcId = o.value("fdcId", 0LL);
                    e.description = o.value("description", "");
                    e.grams = o.value("grams", 0.0);
                    e.kcalPer100g = o.value("kcalPer100g", 0.0);
                    log[date][meal].push_back(e);
                }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Could not read %s: %s\n", DATA_FILE, ex.what());
    }
}

// ======================= USDA API =======================
static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Throws std::runtime_error with a user-readable message on failure.
static json searchUSDA(const std::string& apiKey, const std::string& query, const std::string& dataType) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialise libcurl");

    char* q = curl_easy_escape(curl, query.c_str(), (int)query.size());
    char* dt = curl_easy_escape(curl, dataType.c_str(), (int)dataType.size());
    std::string url = "https://api.nal.usda.gov/fdc/v1/foods/search?query=" + std::string(q) + "&pageSize=25";
    if (dataType != "All") url += "&dataType=" + std::string(dt);
    url += "&api_key=" + apiKey;
    curl_free(q);
    curl_free(dt);

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) throw std::runtime_error(std::string("Network error: ") + curl_easy_strerror(rc));
    if (status == 401 || status == 403) throw std::runtime_error("API key rejected (HTTP " + std::to_string(status) + ")");
    if (status == 429) throw std::runtime_error("Rate limit reached. Wait a bit and try again.");
    if (status != 200) throw std::runtime_error("USDA API returned HTTP " + std::to_string(status));
    try {
        return json::parse(body);
    } catch (...) {
        throw std::runtime_error("Could not parse USDA response");
    }
}

// kcal per 100 g. Prefer nutrient number 208; otherwise any KCAL energy entry
// (Foundation Foods often use Atwater 957/958).
static double extractKcal(const json& food) {
    if (!food.contains("foodNutrients")) return -1;
    double fallback = -1;
    for (const auto& n : food["foodNutrients"]) {
        if (n.value("unitName", "") != "KCAL") continue;
        if (!n.contains("value") || !n["value"].is_number()) continue;
        double v = n["value"].get<double>();
        std::string num = (n.contains("nutrientNumber") && n["nutrientNumber"].is_string())
                              ? n["nutrientNumber"].get<std::string>() : "";
        if (num == "208") return v;
        if (fallback < 0) fallback = v;
    }
    return fallback;
}

static std::vector<FoodHit> parseHits(const json& resp) {
    std::vector<FoodHit> hits;
    if (!resp.contains("foods")) return hits;
    for (const auto& f : resp["foods"]) {
        FoodHit h;
        h.fdcId = f.value("fdcId", 0LL);
        h.description = f.value("description", "");
        h.dataType = f.value("dataType", "");
        h.kcalPer100g = extractKcal(f);
        hits.push_back(h);
    }
    return hits;
}

// ======================= Async search =======================
struct SearchJob {
    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<bool> ready{false};
    std::mutex m;
    std::vector<FoodHit> hits;  // guarded by m
    std::string error;          // guarded by m

    void start(std::string key, std::string query, std::string type) {
        if (worker.joinable()) worker.join();
        busy = true;
        ready = false;
        worker = std::thread([this, key, query, type] {
            std::vector<FoodHit> h;
            std::string err;
            try {
                h = parseHits(searchUSDA(key, query, type));
            } catch (const std::exception& e) {
                err = e.what();
            }
            {
                std::lock_guard<std::mutex> lk(m);
                hits = std::move(h);
                error = err;
            }
            ready = true;
        });
    }
    ~SearchJob() {
        if (worker.joinable()) worker.join();
    }
};

// ======================= UI =======================
struct App {
    Log log;
    Settings settings;
    std::string date = todayStr();

    // search panel state
    char queryBuf[256] = "";
    char keyBuf[128] = "";
    int mealIdx = 0;
    int typeIdx = 0;
    std::vector<FoodHit> hits;
    int selected = -1;
    double grams = 100.0;
    std::string status = "Search for a food to begin.";
    bool pendingImport = false;
    SearchJob job;

    void init() {
        loadAll(log, settings);
        if (settings.apiKey.empty())
            if (const char* e = std::getenv("FDC_API_KEY")) settings.apiKey = e;
        settings.apiKey = trimText(settings.apiKey);
        std::snprintf(keyBuf, sizeof keyBuf, "%s", settings.apiKey.c_str());
    }

    void save() { std::ofstream(DATA_FILE) << toJsonString(); }

    std::string toJsonString() const {
        json j;
        j["settings"] = {{"apiKey", settings.apiKey}, {"goal", settings.goal}};
        j["log"] = json::object();
        for (const auto& [date, meals] : log)
            for (const auto& [meal, entries] : meals)
                for (const auto& e : entries)
                    j["log"][date][meal].push_back({{"fdcId", e.fdcId},
                                                    {"description", e.description},
                                                    {"grams", e.grams},
                                                    {"kcalPer100g", e.kcalPer100g}});
        return j.dump(2);
    }

    static std::string entryText(const Entry& e) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s  -  %.0f g  -  %.0f kcal",
                      e.description.c_str(), e.grams, e.kcal());
        return buf;
    }

    void copyText(const std::string& text, const char* label) {
        ImGui::SetClipboardText(text.c_str());
        SDL_SetClipboardText(text.c_str());
        status = std::string("Copied ") + label + " to clipboard.";
    }

    std::string pasteText() const {
        char* sdlText = SDL_GetClipboardText();
        if (sdlText) {
            std::string result = sdlText;
            SDL_free(sdlText);
            if (!result.empty()) return result;
        }
        const char* t = ImGui::GetClipboardText();
        return t ? t : "";
    }

    void importFromClipboard() {
        const char* raw = ImGui::GetClipboardText();
        if (!raw || !*raw) { status = "Clipboard is empty."; return; }

        try {
            json j = json::parse(raw);
            if (!j.is_object()) throw std::runtime_error("Clipboard JSON must be an object");

            Settings newSettings = settings;
            Log newLog;
            bool hasSettings = false;
            bool hasLog = false;

            if (j.contains("settings")) {
                hasSettings = true;
                const json& s = j["settings"];
                if (!s.is_object()) throw std::runtime_error("settings must be an object");
                if (s.contains("apiKey")) {
                    if (!s["apiKey"].is_string()) throw std::runtime_error("settings.apiKey must be a string");
                    newSettings.apiKey = trimText(s["apiKey"].get<std::string>());
                }
                if (s.contains("goal")) {
                    if (!s["goal"].is_number_integer()) throw std::runtime_error("settings.goal must be an integer");
                    newSettings.goal = s["goal"].get<int>();
                    if (newSettings.goal < 0) throw std::runtime_error("settings.goal must be non-negative");
                }
            }
            if (j.contains("log")) {
                hasLog = true;
                const json& logJ = j["log"];
                if (!logJ.is_object()) throw std::runtime_error("log must be an object");
                for (const auto& [date, meals] : logJ.items()) {
                    if (!meals.is_object()) throw std::runtime_error("Each date entry must be an object");
                    for (const auto& [meal, arr] : meals.items()) {
                        if (std::find(MEALS.begin(), MEALS.end(), meal) == MEALS.end())
                            throw std::runtime_error("Unknown meal: " + meal);
                        if (!arr.is_array()) throw std::runtime_error("Each meal entry must be an array");
                        for (const auto& o : arr) {
                            if (!o.is_object()) throw std::runtime_error("Each logged entry must be an object");
                            Entry e;
                            e.fdcId = o.value("fdcId", 0LL);
                            e.description = o.value("description", "");
                            e.grams = o.value("grams", 0.0);
                            e.kcalPer100g = o.value("kcalPer100g", 0.0);
                            if (e.grams < 0) throw std::runtime_error("Entry grams must be non-negative");
                            newLog[date][meal].push_back(e);
                        }
                    }
                }
            }
            if (!hasSettings && !hasLog)
                throw std::runtime_error("JSON must contain settings and/or log");

            log.swap(newLog);
            settings = newSettings;
            std::snprintf(keyBuf, sizeof keyBuf, "%s", settings.apiKey.c_str());
            pendingImport = false;
            save();
            status = "Imported tracker data from clipboard.";
        } catch (const std::exception& ex) {
            status = std::string("Import failed: ") + ex.what();
        }
    }

    void pollSearch() {
        if (!job.ready.exchange(false)) return;
        if (job.worker.joinable()) job.worker.join();
        std::lock_guard<std::mutex> lk(job.m);
        hits = std::move(job.hits);
        selected = -1;
        if (!job.error.empty()) status = job.error;
        else if (hits.empty()) status = "No results.";
        else status = std::to_string(hits.size()) + " results. Per-100g energy shown.";
        job.busy = false;
    }

    void runSearch() {
        settings.apiKey = trimText(settings.apiKey);
        std::snprintf(keyBuf, sizeof keyBuf, "%s", settings.apiKey.c_str());
        if (settings.apiKey.empty()) { status = "Enter your USDA API key under Settings first."; return; }
        if (queryBuf[0] == '\0') return;
        status = "Searching...";
        job.start(settings.apiKey, queryBuf, DATA_TYPES[typeIdx]);
    }

    // ---------- Left: diary ----------
    void drawDiary() {
        if (ImGui::Button("<")) date = shiftDate(date, -1);
        ImGui::SameLine();
        if (ImGui::Button(">")) date = shiftDate(date, +1);
        ImGui::SameLine();
        if (ImGui::Button("Today")) date = todayStr();
        ImGui::SameLine();
        ImGui::TextUnformatted(prettyDate(date).c_str());
        ImGui::Separator();

        auto& day = log[date];
        double dayTotal = 0;
        for (auto& [meal, v] : day) for (auto& e : v) dayTotal += e.kcal();

        ImGui::SetNextItemWidth(120 * ImGui::GetIO().FontGlobalScale);
        if (ImGui::InputInt("Daily goal (kcal)", &settings.goal, 50, 250)) {
            if (settings.goal < 0) settings.goal = 0;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) save();

        float frac = settings.goal > 0 ? (float)(dayTotal / settings.goal) : 0.f;
        bool over = settings.goal > 0 && dayTotal > settings.goal;
        char overlay[96];
        std::snprintf(overlay, sizeof overlay, "%.0f / %d kcal", dayTotal, settings.goal);
        if (over) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.25f, 0.25f, 1.f));
        ImGui::ProgressBar(frac > 1.f ? 1.f : frac, ImVec2(-1, 0), overlay);
        if (over) ImGui::PopStyleColor();
        if (settings.goal > 0)
            ImGui::Text("%s: %.0f kcal", over ? "Over goal by" : "Remaining",
                        over ? dayTotal - settings.goal : settings.goal - dayTotal);
        ImGui::Spacing();

        if (ImGui::Button("Copy day summary")) {
            std::string text = prettyDate(date) + "\nTotal: " + std::to_string((int)dayTotal) + " kcal\n";
            for (auto& [meal, v] : day) {
                double t = 0;
                for (auto& e : v) t += e.kcal();
                text += "\n" + meal + "  -  " + std::to_string((int)t) + " kcal";
                for (auto& e : v) text += "\n  " + entryText(e);
            }
            copyText(text, "day summary");
        }
        ImGui::Spacing();

        int delMeal = -1, delIdx = -1;
        for (size_t mi = 0; mi < MEALS.size(); ++mi) {
            auto& v = day[MEALS[mi]];
            double t = 0;
            for (auto& e : v) t += e.kcal();
            char header[96];
            std::snprintf(header, sizeof header, "%s  -  %.0f kcal###meal%zu", MEALS[mi].c_str(), t, mi);
            ImGui::PushID((int)mi);
            if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                if (v.empty()) ImGui::TextDisabled("  (nothing logged)");
                for (size_t i = 0; i < v.size(); ++i) {
                    ImGui::PushID((int)i);
                    if (ImGui::SmallButton("C")) copyText(entryText(v[i]), "entry");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) { delMeal = (int)mi; delIdx = (int)i; }
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s  -  %.0f g  -  %.0f kcal", v[i].description.c_str(), v[i].grams, v[i].kcal());
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
        if (delMeal >= 0) {
            auto& v = day[MEALS[delMeal]];
            v.erase(v.begin() + delIdx);
            save();
        }
    }

    // ---------- Right: search / add ----------
    void drawSearch() {
        ImGui::TextUnformatted("Add food");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Settings (USDA API key)")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##key", keyBuf, sizeof keyBuf, ImGuiInputTextFlags_Password);
            if (ImGui::Button("Save key")) {
                settings.apiKey = trimText(keyBuf);
                std::snprintf(keyBuf, sizeof keyBuf, "%s", settings.apiKey.c_str());
                save();
                status = "API key saved.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Paste key")) {
                std::snprintf(keyBuf, sizeof keyBuf, "%s", trimText(pasteText()).c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Free key: fdc.nal.usda.gov/api-key-signup");

            ImGui::Separator();
            ImGui::TextUnformatted("Data");
            if (ImGui::Button("Copy data as JSON")) copyText(toJsonString(), "tracker data");
            ImGui::SameLine();
            if (ImGui::Button("Import from clipboard")) pendingImport = true;
            if (pendingImport) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                   "This replaces all current data with clipboard JSON.");
                if (ImGui::Button("Confirm replace all data")) importFromClipboard();
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) pendingImport = false;
            }
        }

        ImGui::SetNextItemWidth(160 * ImGui::GetIO().FontGlobalScale);
        if (ImGui::BeginCombo("Meal", MEALS[mealIdx].c_str())) {
            for (int i = 0; i < (int)MEALS.size(); ++i)
                if (ImGui::Selectable(MEALS[i].c_str(), i == mealIdx)) mealIdx = i;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(160 * ImGui::GetIO().FontGlobalScale);
        if (ImGui::BeginCombo("Source", DATA_TYPES[typeIdx])) {
            for (int i = 0; i < IM_ARRAYSIZE(DATA_TYPES); ++i)
                if (ImGui::Selectable(DATA_TYPES[i], i == typeIdx)) typeIdx = i;
            ImGui::EndCombo();
        }

        bool busy = job.busy.load();
        ImGui::SetNextItemWidth(-150 * ImGui::GetIO().FontGlobalScale);
        bool enter = ImGui::InputTextWithHint("##q", "e.g. bread", queryBuf, sizeof queryBuf,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Paste")) {
            std::snprintf(queryBuf, sizeof queryBuf, "%s", pasteText().c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        bool click = ImGui::Button("Search");
        ImGui::EndDisabled();
        if ((enter || click) && !busy) runSearch();

        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Spacing();

        float reserve = 110 * ImGui::GetIO().FontGlobalScale;
        float h = ImGui::GetContentRegionAvail().y - reserve;
        if (h < 80) h = 80;
        if (ImGui::BeginChild("results", ImVec2(0, h), ImGuiChildFlags_Borders)) {
            for (int i = 0; i < (int)hits.size(); ++i) {
                char label[512];
                if (hits[i].kcalPer100g >= 0)
                    std::snprintf(label, sizeof label, "%s  [%.0f kcal/100g]###h%d", hits[i].description.c_str(),
                                  hits[i].kcalPer100g, i);
                else
                    std::snprintf(label, sizeof label, "%s  [no energy data]###h%d", hits[i].description.c_str(), i);
                if (ImGui::Selectable(label, selected == i)) selected = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s, FDC ID %lld)", hits[i].description.c_str(),
                                                              hits[i].dataType.c_str(), hits[i].fdcId);
            }
        }
        ImGui::EndChild();

        bool valid = selected >= 0 && selected < (int)hits.size() && hits[selected].kcalPer100g >= 0;
        ImGui::SetNextItemWidth(140 * ImGui::GetIO().FontGlobalScale);
        ImGui::InputDouble("Grams", &grams, 10.0, 50.0, "%.0f");
        if (grams < 0) grams = 0;
        if (valid) ImGui::Text("= %.0f kcal", grams / 100.0 * hits[selected].kcalPer100g);
        else ImGui::TextDisabled("Select a food with energy data.");

        if (selected >= 0 && selected < (int)hits.size()) {
            if (ImGui::Button("Copy selected")) {
                char buf[1024];
                std::snprintf(buf, sizeof buf, "%s  [%.0f kcal/100g]",
                              hits[selected].description.c_str(), hits[selected].kcalPer100g);
                copyText(buf, "food");
            }
            ImGui::SameLine();
        }

        ImGui::BeginDisabled(!valid || grams <= 0);
        std::string btn = "Add to " + MEALS[mealIdx];
        if (ImGui::Button(btn.c_str())) {
            Entry e{hits[selected].fdcId, hits[selected].description, grams, hits[selected].kcalPer100g};
            log[date][MEALS[mealIdx]].push_back(e);
            save();
            status = "Added " + e.description + ".";
        }
        ImGui::EndDisabled();
    }

    void draw() {
        pollSearch();
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("root", nullptr, wf);
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::BeginChild("diary", ImVec2(w * 0.5f - 4, 0), ImGuiChildFlags_Borders)) drawDiary();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("search", ImVec2(0, 0), ImGuiChildFlags_Borders)) drawSearch();
        ImGui::EndChild();
        ImGui::End();
    }
};

// ======================= main =======================
int main(int, char**) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Prefer a native Wayland backend when available so HiDPI and clipboard work
    // correctly instead of running through XWayland (which upscales/blurs).
    const bool preferWayland = !std::getenv("SDL_VIDEODRIVER") && std::getenv("WAYLAND_DISPLAY");
    if (preferWayland) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (preferWayland) {
            SDL_Quit();
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
                return 1;
            }
        } else {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    float scale = 1.0f;
    {
        SDL_Window* probe = SDL_CreateWindow("scale-probe", 1, 1, SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (probe) {
            scale = SDL_GetWindowPixelDensity(probe);
            SDL_DestroyWindow(probe);
        }
    }
#ifndef __APPLE__
    if (scale <= 0.f) scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
#endif
    if (scale <= 0.f || scale < 1.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;

    SDL_Window* window = SDL_CreateWindow("Calorie Tracker", 1100, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    // Let SDL/render backend scale the base-resolution UI to the compositor density.
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
#ifdef FDC_FONT_PATH
    {
        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        if (!io.Fonts->AddFontFromFileTTF(FDC_FONT_PATH, 16.0f, &cfg))
            io.Fonts->AddFontDefault();
    }
#else
    io.Fonts->AddFontDefault();
#endif
    ImGui::GetStyle().ScaleAllSizes(1.0f);
    io.FontGlobalScale = 1.0f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        App app;
        app.init();

        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                ImGui_ImplSDL3_ProcessEvent(&ev);
                if (ev.type == SDL_EVENT_QUIT) running = false;
                if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))
                    running = false;
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(20);
                continue;
            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();

            SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.12f, 1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }
    }  // App destructor joins any running search thread

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    curl_global_cleanup();
    return 0;
}
