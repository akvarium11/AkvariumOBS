<p align="center">
  <img src="packaging/branding/banner.png" alt="AkvariumOBS — форк OBS Studio для стримов" width="100%">
</p>

<p align="center">
  <a href="COPYING"><img src="https://img.shields.io/badge/license-GPL--2.0-111?style=flat-square" alt="License GPL-2.0"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20x64-111?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/base-OBS%20Studio%2032.2.2-111?style=flat-square" alt="OBS Studio 32.2.2">
  <img src="https://img.shields.io/badge/speech-whisper.cpp%2C%20offline-111?style=flat-square" alt="whisper.cpp offline">
  <img src="https://img.shields.io/badge/C%2B%2B-Qt6%20%2F%20CMake-111?style=flat-square" alt="C++ Qt6 CMake">
</p>

<p align="center">
  Личный форк OBS Studio 32.2.2: реплей по голосовой команде, отдельный миксер для каждой сцены<br>
  и автоматическое переключение сцен по активному приложению.
</p>

---

## Что это

AkvariumOBS — не плагин и не обёртка, а полноценный форк исходников OBS Studio 32.2.2.
Три вещи, которые мне были нужны в стриме и которых нет в апстриме:

| | Возможность | Зачем |
|---|---|---|
| 🎙 | **Голосовые команды для Replay Buffer** | сказать «сделай реплей» вместо хоткея — распознавание полностью офлайн |
| 🎚 | **Состояние микшера на сцену** | микрофон заглушён в одной сцене и жив в другой, автоматически |
| 🪟 | **Привязка сцен к приложениям** | сцена меняется, когда игра вышла в фокус, свернулась или закрылась |

Продуктовое имя, заголовок окна, `AkvariumOBS.exe`, версионный ресурс Windows, иконки
(exe, окно, трей) и тексты своего UI перебрендированы. Внутренние идентификаторы OBS,
пути конфигов и ABI плагинов не тронуты — сторонние плагины апстрима работают как раньше.

---

## Возможности

### 🎙 Голосовые команды для Replay Buffer

**Настройки → Вывод → Replay Buffer → Voice Commands (Replay Buffer).**

- Фразы — по одной в строке, русский или английский. Больше ничего настраивать не нужно.
- Пресет модели выбирается один раз: `Fast` (tiny, ~75 МБ), `Balanced` (base, ~142 МБ),
  `Accurate` (small, ~466 МБ). Кнопка **Download / verify model** кладёт модель в
  `<config>/AkvariumOBS/voice/`.
- Микрофон: *Auto* берёт активный источник захвата звука текущей сцены, либо конкретный
  источник OBS, либо любое устройство Windows (для output-устройств — loopback).
- По умолчанию движок работает только пока запущен Replay Buffer.

Распознавание — whisper.cpp, целиком локально, никакой сети. Задержка зафиксирована
пресетом, а не ползунком: WASAPI 16 кГц моно, адаптивный шумовой гейт, хвост речи 350 мс,
потоков — половина логических ядер, `audio_ctx 768`, greedy без контекста. На Ryzen 5 5500
команда длиной 1.4 с распознаётся за ~0.7 с. Модель остаётся в памяти между запусками буфера.
GPU-офлоад whisper идёт через Vulkan, если сборка и драйвер это умеют; иначе тихий откат на CPU.

Сопоставление команд терпит ошибки распознавания (`сделай реплеи` → `сделай реплей`,
`safe replay` → `save replay`) и отбрасывает длинную постороннюю речь.

### 🎚 Состояние микшера на сцену

Работает само: при переключении сцены состояние уходящей сцены сохраняется в её JSON,
состояние входящей — применяется. Охвачены все источники из Audio Mixer, включая глобальные
устройства вроде `Mic/Aux` и `Desktop Audio`.

Правый клик по сцене:

- **Remember volume and mute for this scene** — включить/выключить запоминание;
- **Capture current audio state for this scene** — перезаписать снимок текущим состоянием.

### 🪟 Привязка сцен к приложениям

Док **Scene Binding to Applications** (справа). Правило — это exe, условие, целевая сцена
и флаг «включено».

| Условие | Срабатывает, когда |
|---|---|
| In foreground | приложение владеет активным окном |
| Running (foreground or background) | процесс существует |
| Running but not in foreground | процесс существует и не в фокусе |
| Not running (closed) | процесса нет |

Правила проверяются сверху вниз каждые 700 мс, побеждает первое совпавшее, сцена
переключается только при смене совпавшего правила. Хранятся в `user.ini`, секция
`[AkvariumAppScenes]`.

### Мелочи, без которых неудобно

- **Импорт профилей OBS при первом запуске** — если найден `%APPDATA%\obs-studio`, форк
  предлагает перенести профили и коллекции сцен, чтобы стартовать не с пустого холста.
  Спрашивает один раз.
- **Раскладка доков запоминается** — свой док создаётся до восстановления сохранённого
  layout, поэтому его позиция не слетает.
- **UI на русском** — включая добавленные экраны.
- **Своя иконка** во всех состояниях трея.

---

## Установка

Готовые артефакты собираются из `packaging/` (см. [packaging/README.md](packaging/README.md)):

```text
AkvariumOBS-32.2.2-Windows-x64-Installer.exe   установщик (NSIS)
AkvariumOBS-32.2.2-Windows-x64-Portable.zip    портативная сборка
```

- Установщик ставит в выбранную папку, пишет `HKLM\Software\AkvariumOBS`, добавляет пункт
  в «Удаление программ» и ярлыки. Тихая установка: `installer.exe /S /D=D:\path`.
- Настройки пользователя живут в `%APPDATA%\obs-studio` — том же каталоге, что и у OBS Studio,
  поэтому профили переносятся один в один.
- В portable-сборку уже вложены VC++ runtime, OpenMP, Vulkan loader и модель распознавания,
  отдельно доставлять ничего не нужно.

Запуск собранной версии из дерева сборки:

```bash
build_x64/rundir/RelWithDebInfo/bin/64bit/AkvariumOBS.exe --portable
```

---

## Быстрый старт

**Реплей по голосу**

1. Настройки → Вывод → включить Replay Buffer, зайти в **Voice Commands**.
2. Вписать фразы: `сделай реплей`, `save replay` — по одной в строке.
3. Выбрать модель (`Balanced` — разумный дефолт), нажать **Download / verify model**.
4. Микрофон оставить на *Auto*. Запустить буфер и сказать фразу.

**Свой миксер в каждой сцене**

1. Настроить звук в первой сцене → правый клик по сцене → **Capture current audio state**.
2. Переключиться на другую сцену, настроить звук иначе — снимок создастся сам.
3. Правый клик → **Remember volume and mute for this scene** отключает поведение для сцены.

**Сцена под приложение**

1. Открыть док **Scene Binding to Applications**.
2. Добавить правило: exe игры, условие `In foreground`, нужная сцена.
3. Второе правило: та же игра, `Not running (closed)` → сцена «стол».

---

## Сборка из исходников

Нужно:

- Visual Studio 2022 Build Tools с workload **Desktop development with C++** и компонентом **C++ ATL**
- Windows 11 SDK 10.0.26100
- CMake 3.28+ (генератор Visual Studio 17 2022)
- ~6 ГБ на зависимости, ~4 ГБ на дерево сборки

```bash
cmake -S . -B build_x64 -G "Visual Studio 17 2022" -A x64 -DENABLE_BROWSER=ON
cmake --build build_x64 --config RelWithDebInfo --parallel
```

Зависимости (obs-deps, Qt6, CEF) скачиваются в `.deps` при первой конфигурации и сверяются
по хешам из `CMakePresets.json`. Если CMake не может проверить TLS (нет CA-стора),
конфигурировать с `-DCMAKE_TLS_VERIFY=0`.

---

## Тесты

```bash
# сопоставление фраз, модель не нужна
cmake --build build_x64 --config RelWithDebInfo --target akvarium-phrase-match-test
build_x64/frontend/RelWithDebInfo/akvarium-phrase-match-test.exe

# распознавание на wav-файле
cmake --build build_x64 --config RelWithDebInfo --target akvarium-voice-selftest
akvarium-voice-selftest <model.bin> <audio.wav> "save replay|сделай реплей"
```

---

## Где что лежит

| Путь | Что внутри |
|---|---|
| `frontend/utility/AkvariumVoice.*` | движок распознавания, WASAPI-захват, настройки голоса |
| `frontend/utility/AkvariumPhraseMatch.hpp` | устойчивое к ошибкам сопоставление фраз |
| `frontend/utility/AkvariumSceneAudio.*` | снимки микшера на сцену |
| `frontend/docks/AkvariumAppScenes.*` | док привязки сцен к приложениям |
| `frontend/utility/AkvariumImport.*` | импорт профилей OBS при первом запуске |
| `libobs/` | хранение per-scene аудио-состояния в JSON сцены |
| `deps/whisper.cpp` | вендоренный движок распознавания |
| `packaging/` | NSIS-установщик, portable zip, README по релизам |
| `packaging/branding/` | баннеры, генератор `make-banner.py` |

Подробное техническое описание фич, настроек и ограничений — в
[README.AkvariumOBS.md](README.AkvariumOBS.md). Документация апстрима OBS — в
[README.rst](README.rst) и на [obsproject.com/docs](https://obsproject.com/docs).

---

## Совместимость

- Только **Windows x64**; сборка под Linux/macOS не поддерживается.
- Версия-база — OBS Studio **32.2.2**, плагины, собранные под неё, грузятся без пересборки.
- Конфиги, профили и коллекции сцен полностью совместимы с OBS Studio: можно ходить
  туда-обратно между приложениями.
- Для GPU-офлоада whisper нужен рабочий Vulkan-драйвер; без него распознавание идёт на CPU.

---

## Лицензия

GPLv2, как и OBS Studio. Оригинальные copyright-уведомления и `COPYING` сохранены,
`About` указывает форк и версию апстрима. Поскольку проект под GPLv2, при распространении
бинарников исходники должны быть доступны.

Благодарности: [OBS Studio](https://obsproject.com) — база форка,
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) — распознавание речи,
[Qt](https://www.qt.io) и [CEF](https://github.com/chromiumembedded/cef) — то, на чём стоит фронтенд.
