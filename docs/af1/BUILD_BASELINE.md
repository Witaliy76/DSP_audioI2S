# AF-1.1: контрольная сборка DSP_audioI2S

Компиляция текущей библиотеки на ESP32-S3 с PSRAM и внешним I2S DAC. Исходники `src/**` и `examples/**` не менялись. Устройство не прошивалось.

## База

| | |
|---|---|
| Репозиторий | https://github.com/Witaliy76/DSP_audioI2S |
| Upstream | https://github.com/schreibfaul1/ESP32-audioI2S |
| Принятая база AF-1.0 | `3b0fe2682c01c64ebe8cb2980e6f9e7608884fe4` (`add fkt. count_of`) |
| `main`, `origin/main`, `night_build`, `origin/night_build`, `upstream/master` | тот же SHA |
| Ancestry `main...upstream/master` | `0 0` |
| Ветка этой работы | `feature/dsp-audioi2s-af1-build-baseline` |

Новых upstream-коммитов после базы нет. База автоматически не сдвигалась.

`library.json` объявляет frameworks `arduino` и `espidf`. Эта проверка собирает только Arduino. ESP-IDF framework не собирался, поэтому запись в `library.json` сама по себе не считается доказательством.

## Окружение

Глобальный `pio` в PATH отсутствовал. Пакеты PlatformIO уже лежали в `%USERPROFILE%\.platformio` и были переиспользованы. Core поставлен в изолированный venv проекта, без обновления глобальных инструментов и без изменений YoRadio.

| Компонент | Версия |
|---|---|
| PlatformIO Core | 6.2.0, `validation/baseline/.venv` |
| Python venv | CPython 3.13.13 |
| platform | pioarduino Espressif 32 **55.3.312** (`platform-espressif32` 55.03.312) |
| board | `esp32-s3-devkitc1-n8r8`: ESP32-S3-DevKitC-1-N8R8, 8 MB flash quad, 8 MB PSRAM octal, `-DBOARD_HAS_PSRAM`, `memory_type=qio_opi` |
| framework | `framework-arduinoespressif32` **3.3.12** (Arduino-ESP32) |
| Arduino libs | `framework-arduinoespressif32-libs` **5.5.5+sha.b774170ff46** |
| ESP-IDF | **v5.5.5** (`b774170ff46`), из `esp32s3/versions.txt` и `flags/defines` (`-DIDF_VER=\"v5.5.5\"`) |
| Compiler | `xtensa-esp-elf-g++` (crosstool-NG esp-14.2.0_20260121) **14.2.0**; пакет `toolchain-xtensa-esp-elf` 14.2.0+20260121 |
| esptool | 5.4.0 |
| GDB | `tool-xtensa-esp-elf-gdb` 17.1.0+20260402 |

Внешние library dependencies в `library.json` пустые. В граф сборки вошли компоненты ядра Arduino 3.3.12, которые подключает сама библиотека: FS, FFat, Network, NetworkClientSecure, SPI, SD, SD_MMC, WiFi. Заголовок `esp_dsp.h` взят из включённого дерева `framework-arduinoespressif32-libs/esp32s3/include/espressif__esp-dsp`. В `esp32s3/versions.txt` этого дерева указан `espressif__esp-dsp` **1.8.0**.

Fixture добавляет `-DCORE_DEBUG_LEVEL=0`. Это значение по умолчанию Arduino IDE (`platform.txt`: `build.code_debug=0`, флаг `-DCORE_DEBUG_LEVEL={build.code_debug}`). pioarduino этот макрос сам не передаёт.

## Повторная сборка

Из корня репозитория, тем же изолированным core:

```text
validation/baseline/.venv/Scripts/pio.exe run -d validation/baseline -e esp32s3-psram
validation/baseline/.venv/Scripts/pio.exe run -d validation/baseline/sd -e esp32s3-psram
validation/baseline/.venv/Scripts/pio.exe run -d validation/baseline/sd_mmc -e esp32s3-psram
```

Если venv ещё нет, его можно создать заново, не ставя PlatformIO в системный Python:

```text
uv venv --python 3.13 validation/baseline/.venv
uv pip install --python validation/baseline/.venv/Scripts/python.exe platformio==6.2.0
```

`pio run` только компилирует. Upload и monitor не запускать.

Каталоги `.pio/` и `.venv/` перечислены в `validation/baseline/.gitignore`.

## Результаты

Плата одна: `esp32-s3-psram`. Успех ниже означает только компиляцию и линковку.

| Сборка | Исходник | Результат | Время | RAM | Flash |
|---|---|---|---|---|---|
| API fixture | `validation/baseline/src/main.cpp` | SUCCESS | 98.72 с | 43840 / 327680 (13.4%) | 1483308 / 3342336 (44.4%) |
| SD | оригинальный `examples/I2Saudio_SD/I2Saudio_SD.cpp` | SUCCESS | 95.13 с | 43976 / 327680 (13.4%) | 1533396 / 3342336 (45.9%) |
| SD_MMC | оригинальный `examples/I2Saudio_SD_MMC/I2Saudio_SD_MMC.cpp` | SUCCESS | 99.62 с | 44256 / 327680 (13.5%) | 1560432 / 3342336 (46.7%) |

API fixture вызывает `setPinout`, `connecttohost`, `loop`, `setVolume`, `setTone`, `setVolumeCurve` и `Audio::audio_info_callback`. Обе weak PCM-функции определены с сигнатурами из `Audio.h`: `audio_process_raw_samples(int32_t*, int16_t)` и `audio_process_i2s(int32_t*, int16_t, bool*)`. URL `http://example.invalid/baseline.mp3` — заглушка, не секрет и не живой поток.

## Локальная библиотека

`lib_deps = symlink://../..` (для примеров `symlink://../../..`) указывает на корень этого репозитория. Registry-копия ESP32-audioI2S не устанавливалась.

Зависимости компилятора:

- `Audio.cpp.o` собран из `C:/Github/Yoradio_lvgl/DSP_AudioI2S/src/Audio.cpp` и `src/Audio.h`
- SD: `examples/I2Saudio_SD/I2Saudio_SD.cpp` включает тот же `src/Audio.h`
- SD_MMC: `examples/I2Saudio_SD_MMC/I2Saudio_SD_MMC.cpp` включает тот же `src/Audio.h`

Объектный каталог сборки называется `libe88/DSP_AudioI2S`, по имени локального каталога. `git diff` против базы для `src/**`, `examples/**`, `LICENSE`, `README.md`, `library.json` и `library.properties` пустой.

## Ограничения

- HTTP/HTTPS, SD/SD_MMC и звук на железе не проверялись. Компиляция не является PASS для воспроизведения.
- ESP-IDF framework из `library.json` не собирался.
- Оригинальные примеры сохранены как есть, включая их номера GPIO. Сборка не доказывает, что эти пины подходят к конкретной плате ESP32-S3.
- Первый проход без `-DCORE_DEBUG_LEVEL` остановился в `src/Audio.h:835`: `'CORE_DEBUG_LEVEL' was not declared in this scope`. Это разрыв между Arduino IDE и pioarduino, не отсутствующая библиотека. Fixture задаёт тот же уровень `0`. Исходник библиотеки не менялся. Возможное последующее изменение библиотеки — запасное `#ifndef CORE_DEBUG_LEVEL` в `AUDIO_LOG_IMPL`; сейчас оно не вносилось.
- FLAC, Vorbis, reconnect, Engine, PCM API и DSP не трогались.
- `main` и `night_build` не изменялись. Push, release и прошивка не выполнялись.
