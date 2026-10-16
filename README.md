# Calorie Tracker GUI (SDL3 + Dear ImGui)

Search USDA FoodData Central, enter grams, and log meals per day with a daily calorie goal.

## Prerequisites
- A C++17 compiler and CMake 3.16+
- libcurl development files:
  - Ubuntu/Debian: `sudo apt install build-essential cmake libcurl4-openssl-dev`
    plus SDL3's Linux build deps, e.g.
    `sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libgl1-mesa-dev libdecor-0-dev`
  - macOS: `brew install cmake curl`
  - Windows: `vcpkg install curl:x64-windows`, then pass
    `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` to CMake
- Internet access during the first configure (CMake downloads SDL3, Dear ImGui, nlohmann/json)

## Build
```
cmake -B build
cmake --build build --config Release
```

## Run
```
./build/calorie_tracker_gui          # Windows: build\Release\calorie_tracker_gui.exe
```
Run it from the folder where you want `tracker_data.json` to be stored.

## API key
Get a free key at https://fdc.nal.usda.gov/api-key-signup. Either paste it into
the "Settings" section in the app (saved to `tracker_data.json`), or set the
`FDC_API_KEY` environment variable before launching.

## Using it
1. Pick a meal, a source (Foundation is the default), type a food, press Enter or Search.
2. Click a result, enter grams, press "Add to <meal>".
3. The left panel shows the day's meals, totals, and progress toward your goal. Use `<` `>` to change day, `X` to remove an entry.

Calories = grams / 100 x kcal per 100 g.
