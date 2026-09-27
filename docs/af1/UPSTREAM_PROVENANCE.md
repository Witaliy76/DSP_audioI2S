# Происхождение DSP_audioI2S

`DSP_audioI2S` — самостоятельный публичный community fork. Это не официальный выпуск Wolle и не замена репозитория schreibfaul1.

## Upstream

| | |
|---|---|
| Название | ESP32-audioI2S |
| URL | https://github.com/schreibfaul1/ESP32-audioI2S |
| Автор | schreibfaul1 (Wolle) |
| База форка | `3b0fe2682c01c64ebe8cb2980e6f9e7608884fe4` (`add fkt. count_of`, 2026-09-27) |
| Ближайший tag | `4.0.0`. База не помечена отдельным tag и описывается как `4.0.0-64-g3b0fe26` |
| Начало fork baseline | 2026-09-27 |

## Версии

Package version форка — `0.1.0`. Это версия манифестов `DSP_audioI2S`, а не номер линии Wolle.

Runtime-строка `audioI2SVers` в `src/Audio.cpp` остаётся upstream-текстом `Version 4.0.0t`. Исходники аудио в этом срезе не менялись, поэтому package `0.1.0` и runtime `4.0.0t` временно различаются. `4.0.0t` относится к принятому upstream baseline, а не к новому релизу форка.

## Лицензия и attribution

Лицензия остаётся GPL-3.0. Файл `LICENSE` не изменялся. schreibfaul1 сохранён как original author. Maintainer форка — Vitaly Plaksin (Witaliy76).

## Remotes

| Remote | Репозиторий | Роль |
|---|---|---|
| `origin` | https://github.com/Witaliy76/DSP_audioI2S | этот fork |
| `upstream` | https://github.com/schreibfaul1/ESP32-audioI2S | оригинальный проект Wolle |

Обновление upstream: fetch → смысловой разбор изменений → сборка совместимости → регрессия radio. База не сдвигается автоматически только потому, что в upstream появился новый commit.

## Принятая цепочка

| Срез | Commit | Содержание |
|---|---|---|
| AF-1.0 | `3b0fe2682c01c64ebe8cb2980e6f9e7608884fe4` | принятая upstream-база, без изменений аудиокода форка |
| AF-1.1 | `a96ced86008514cc38509d3f381ccdb9ec9e7113` | контрольная сборка ESP32-S3; `night_build` принял её fast-forward |

Относительно upstream baseline нет изменений runtime и поведения аудио. `src/**`, декодеры, примеры и `LICENSE` остаются исходными.

## Ветки

- `main` — стабильная принятая версия. Сейчас это ещё чистая база AF-1.0.
- `night_build` — кандидат для интеграции и проверки. Сейчас на AF-1.1.
- `feature/*` и `fix/*` — отдельные срезы. Они не являются стабильными релизами.

## Дальше

Запланированные зоны, которых в дереве ещё нет: устойчивость streaming/reconnect, modern core, каноническая PCM-граница, события и диагностика, переиспользуемые DSP-блоки. Обычный I2S path и `#include <Audio.h>` / class `Audio` сохраняются как цель source compatibility. Совместимость бинарного ABI не обещается.

`CMakeLists.txt` в этом срезе не менялся: его семантика ESP-IDF/component ещё не проходила контрольную сборку.
