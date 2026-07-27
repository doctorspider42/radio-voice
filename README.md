# RadioVoice

Procesor sygnału mikrofonowego czasu rzeczywistego dla Windows: bramka szumów,
korekcja graficzna, limiter i **łańcuch wtyczek VST3**, z wyjściem kierowanym na
urządzenie wirtualne, które inne aplikacje widzą jako mikrofon.

Napisany w C++20, bez frameworków audio — backendy WASAPI i DirectSound są
własne, interfejs oparty o Dear ImGui i Direct3D 11.

---

## Jak to działa

```
mikrofon ──> WASAPI/ASIO/DirectSound capture
                     │
                     ▼
              bufor pierścieniowy (lock-free)
                     │
                     ▼
        resampler kompensujący dryf zegarów
                     │
                     ▼
   gain ──> [ łańcuch: bramka | EQ | VST3 | VST3 ... ] ──> gain ──> limiter
                     │
                     ▼
        WASAPI/ASIO/DirectSound render
                     │
                     ▼
      urządzenie wirtualne  ──>  Discord / OBS / Teams / …
```

Wejście i wyjście to **dwa niezależne zegary**. Nawet gdy oba pracują nominalnie
przy 48 kHz, realnie różnią się o kilkadziesiąt do kilku tysięcy ppm, więc jeden
bufor po minutach by się przepełnił, a drugi zagłodził. Między nimi siedzi
resampler o zmiennym współczynniku sterowany regulatorem PI, który utrzymuje
zapełnienie bufora na zadanym poziomie. Aktualną korekcję widać w pasku
statusu jako `drift`.

Cały DSP wykonuje się na wątku urządzenia wyjściowego — tak jak w DAW. Osobny
wątek roboczy odciążyłby łańcuch od terminu sprzętowego, ale kosztem kolejnego
bufora opóźnienia w torze, który użytkownik odsłuchuje na żywo.

---

## Wymagania

- Windows 10 1903+ lub Windows 11 (x64)
- CMake ≥ 3.24, Ninja
- Kompilator: **MinGW-w64 GCC ≥ 13** albo **MSVC 2019+**
- Sterownik graficzny z Direct3D 11 (feature level 10.0; awaryjnie WARP)

Zależności (Dear ImGui, nlohmann/json, VST3 SDK) pobiera CMake na etapie
konfiguracji — nic nie jest wersjonowane w repo. Potrzebny jest dostęp do sieci
przy pierwszym `cmake -B`.

## Budowanie

```bash
cmake --preset mingw
cmake --build --preset mingw
```

Wynik: `build/bin/RadioVoice.exe`.

Warianty:

```bash
cmake --preset msvc          # Visual Studio zamiast MinGW
cmake --preset no-vst3       # bez hosta VST3 — usuwa jedyną zależność copyleft
```

### ASIO

SDK Steinberga nie może być redystrybuowane, więc trzeba je pobrać ręcznie
z <https://www.steinberg.net/developers/> i wskazać:

```bash
cmake --preset mingw -DRV_ENABLE_ASIO=ON -DRV_ASIO_SDK_DIR=C:/asiosdk
```

Bez tego lista urządzeń i tak pokazuje zainstalowane sterowniki ASIO —
wyszarzone, z wyjaśnieniem, czego brakuje — zamiast udawać, że ich nie ma.

---

## Wirtualne wyjście

Windows nie ma wbudowanego sposobu, by aplikacja udostępniła strumień audio
innym aplikacjom jako mikrofon. Wymaga to sterownika. Są dwie drogi:

### A. Gotowy wirtualny kabel (działa od razu)

Zainstaluj [VB-CABLE](https://vb-audio.com/Cable/), a następnie:

1. w RadioVoice ustaw **Output** na `CABLE Input (VB-Audio Virtual Cable)`
2. w Discordzie / OBS / Teams wybierz mikrofon `CABLE Output (VB-Audio Virtual Cable)`

Aplikacja wykrywa zainstalowane kable wirtualne sama i przy pierwszym
uruchomieniu ustawia wyjście na `CABLE Input`, jeśli je znajdzie. Gdy żadnego
nie ma, panel I/O tłumaczy dlaczego i linkuje do instalatora.

### B. Własny sterownik (`driver/`)

W katalogu [`driver/`](driver/) leży sterownik jądra, który tworzy parę
endpointów `RadioVoice Output` / `RadioVoice Microphone` i wewnętrznie pętli
jeden na drugi — funkcjonalny odpowiednik VB-CABLE, bez zewnętrznej zależności.

Instalacja wymaga **wyłączonego Secure Boot i włączonego trybu testowego**
podpisywania, albo komercyjnego certyfikatu EV. Szczegóły, kompletna procedura
budowania i lokalnego podpisywania: [`driver/README.md`](driver/README.md).

---

## Interfejs

- **Audio I/O** — backend, urządzenia, tryb dzielony/wyłączny, częstotliwość,
  rozmiar bloku, plus na żywo wynegocjowany format i rozmiar bufora
- **Equalizer** — 10 pasm ISO (skrajne jako półki), filtry HP/LP 24 dB/okt,
  krzywa odpowiedzi na tle widma wejścia; uchwyty pasm ciągnie się myszą,
  kółko nad uchwytem zmienia Q
- **Noise Gate** — próg, zakres, histereza, atak/hold/release, lookahead oraz
  filtr HP w torze detektora
- **Output Limiter** — brickwall z lookahead, chroni wirtualny kabel przed
  przesterowaniem
- **Processing Chain** — bramka, EQ i wtyczki VST3 w jednej, przestawialnej
  liście; każdy element z osobnym bypassem, wtyczki z własnym oknem edytora
  albo listą parametrów

Konfiguracja, łańcuch i stan wtyczek zapisują się w
`%APPDATA%\RadioVoice\config.json`. Log: `%APPDATA%\RadioVoice\radiovoice.log`.

### Skanowanie wtyczek

Standardowe katalogi VST3 skanowane są w tle przy starcie, wyniki cache'owane
w `plugins.json`. Wtyczka potrafi wywrócić proces przy ładowaniu, więc przed
każdą próbą skaner zapisuje jej ścieżkę do pliku wartowniczego i kasuje go po
sukcesie. Jeśli plik przetrwa restart, ta wtyczka jest wpisywana na czarną
listę zamiast być ładowana ponownie — jedna wroga wtyczka nie zablokuje
uruchomienia aplikacji. Czarną listę można wyczyścić z okna „Add plugin”.

---

## Co zostało zweryfikowane

Sprawdzone na tej maszynie, na realnym sprzęcie i realnych wtyczkach:

- kompilacja i linkowanie pod MinGW-w64 GCC 16.1 (MSVC nie był dostępny)
- otwarcie strumieni WASAPI shared w obie strony, praca silnika, mierniki
- skan 20 pakietów VST3 → 16 użytecznych wtyczek (Focusrite, Softube,
  Native Instruments, Klevgrand, IK Multimedia)
- załadowanie wtyczek do łańcucha, przetwarzanie dźwięku, odtworzenie
  łańcucha z zapisanej konfiguracji
- hostowanie natywnego okna edytora VST3 (`IPlugView`) — Softube Saturation
  Knob renderuje się z działającymi miernikami
- automatyczne wykrycie VB-CABLE i ustawienie go jako wyjście

Czego **nie** udało się zweryfikować w tym środowisku:

- **MSVC** — brak workloadu C++; CMake go obsługuje, ale build nie był uruchomiony
- **ASIO** — SDK wymaga ręcznego pobrania; kod skompilowany nigdy nie był
- **WASAPI exclusive** i **DirectSound** — ścieżki napisane, ale nie uruchomione
- **sterownik jądra** — brak WDK na tej maszynie, patrz `driver/README.md`
- interakcja myszą z GUI — automatyzacja nie potrafiła dostarczyć kliknięć do
  okna (przechwytywał je Explorer), więc klikanie sprawdź proszę sam

---

## Licencja

**GPL-3.0-or-later** — konsekwencja linkowania VST3 SDK bez umowy ze Steinbergiem.
Szczegóły i rozbicie na zależności: [NOTICE.md](NOTICE.md).

Build z `-DRV_ENABLE_VST3=OFF` nie zawiera żadnej zależności copyleft.
