# Instrukcja: zbudowanie i uruchomienie

Od zera do przetworzonego mikrofonu w Discordzie.

Wszystkie polecenia w tym pliku były uruchomione na tej maszynie — poza tymi,
które wymagają restartu systemu; te są wyraźnie oznaczone.

---

## Spis

1. [Narzędzia](#1-narzędzia)
2. [Aplikacja](#2-aplikacja)
3. [Wirtualne wyjście — droga A: VB-CABLE](#3a-wirtualne-wyjście--droga-a-vb-cable)
4. [Wirtualne wyjście — droga B: własny sterownik](#3b-wirtualne-wyjście--droga-b-własny-sterownik)
5. [Konfiguracja w Discordzie / OBS / Teams](#4-konfiguracja-w-aplikacjach-docelowych)
6. [Codzienne użycie](#5-codzienne-użycie)
7. [Gdy coś nie działa](#6-gdy-coś-nie-działa)
8. [Cofnięcie wszystkiego](#7-cofnięcie-wszystkiego)

---

## 1. Narzędzia

### Do samej aplikacji

| Co | Po co |
|---|---|
| CMake ≥ 3.24 | system budowania |
| Ninja | generator |
| MinGW-w64 GCC ≥ 13 **albo** MSVC 2019+ | kompilator |
| Git | CMake pobiera nim zależności |

Przez [scoop](https://scoop.sh):

```powershell
scoop install cmake ninja mingw git
```

Zależności (Dear ImGui, nlohmann/json, VST3 SDK) pobiera CMake sam przy
pierwszej konfiguracji. Potrzebny jest dostęp do sieci — potem już nie.

### Dodatkowo do sterownika

```powershell
winget install --id Microsoft.WindowsSDK.10.0.26100 --exact
winget install --id Microsoft.WindowsWDK.10.0.26100 --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override `
  "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools"
```

Instalacje SDK i WDK są maszynowe, więc wyskoczy UAC. Sterownik jądra wymaga
MSVC — MinGW tu nie wystarczy.

> **Na Twojej maszynie to już jest zainstalowane.** SDK 10.0.26100, WDK
> 10.0.26100 i Build Tools 2022 z toolsetem C++ (MSVC 19.44) doinstalowałem w
> trakcie pracy nad sterownikiem.

---

## 2. Aplikacja

### Build

```powershell
cd F:\Projects\radio-voice
cmake --preset mingw
cmake --build --preset mingw
```

Pierwsza konfiguracja trwa ~35 s (klonowanie ImGui i VST3 SDK), build ~1 min.
Wynik: **`build\bin\RadioVoice.exe`**, ok. 28 MB.

Inne warianty:

```powershell
cmake --preset msvc          # Visual Studio zamiast MinGW
cmake --preset mingw-debug   # z symbolami i asercjami
cmake --preset no-vst3       # bez hosta wtyczek — usuwa jedyną zależność copyleft
```

Z obsługą ASIO (SDK trzeba pobrać ręcznie ze strony Steinberga):

```powershell
cmake --preset mingw -DRV_ENABLE_ASIO=ON -DRV_ASIO_SDK_DIR=C:/asiosdk
cmake --build --preset mingw
```

### Pierwsze uruchomienie

```powershell
.\build\bin\RadioVoice.exe
```

Co się dzieje samo:

- jako **wejście** ustawia domyślny mikrofon systemowy
- jako **wyjście** — kabel wirtualny, jeśli jakiś znajdzie (`CABLE Input` ma
  pierwszeństwo); jeśli nie, panel I/O wyjaśni czego brakuje
- startuje przetwarzanie i skanuje wtyczki VST3 w tle
- do łańcucha wkłada bramkę szumów i korektor

Pliki, które tworzy:

```
%APPDATA%\RadioVoice\config.json        ustawienia, łańcuch, stan wtyczek
%APPDATA%\RadioVoice\plugins.json       cache skanu wtyczek
%APPDATA%\RadioVoice\radiovoice.log     log — pierwsza rzecz do sprawdzenia
```

Skasowanie `config.json` przywraca stan fabryczny.

### Sprawdzenie, że działa

W pasku u góry powinno być zielone **RUNNING**, a przy mówieniu do mikrofonu
miernik **INPUT** na dole ma się ruszać. Jeśli stoi — zajrzyj do
[sekcji 6](#6-gdy-coś-nie-działa).

---

## 3A. Wirtualne wyjście — droga A: VB-CABLE

Najszybsza. Nic nie zmienia w zabezpieczeniach systemu.

1. Pobierz i zainstaluj [VB-CABLE](https://vb-audio.com/Cable/) (instalator
   uruchom jako administrator, potem restart).
2. W RadioVoice ustaw **Output** na `CABLE Input (VB-Audio Virtual Cable)`.
   Jeśli aplikacja już działała, kliknij **Restart audio** w pasku u góry.

Gotowe — przejdź do [sekcji 4](#4-konfiguracja-w-aplikacjach-docelowych).

---

## 3B. Wirtualne wyjście — droga B: własny sterownik

Bez zewnętrznych zależności, ale wymaga obniżenia zabezpieczeń maszyny i
dwóch restartów.

> **Przeczytaj najpierw.** Sterownik kompiluje się czysto i cały łańcuch
> podpisywania jest sprawdzony, ale **nigdy nie był załadowany** — to wymaga
> restartu w trybie testowym, którego nie robiłem. Pierwsze uruchomienie zrób
> na maszynie wirtualnej albo z debuggerem jądra. `driver/README.md` ma listę
> miejsc, które najpewniej będą wymagały poprawek.

### Krok 1 — zbuduj

```powershell
cd F:\Projects\radio-voice\driver
.\build.ps1 -Configuration Release
```

Wynik: `driver\build\Release\RadioVoiceAudio.sys` + `.inf`.

### Krok 2 — wyłącz Secure Boot

W firmware (UEFI). Bez tego następny krok zamelduje sukces, ale ustawienie nie
zadziała — Secure Boot blokuje zmianę polityki podpisów.

Sprawdzenie stanu (PowerShell **jako administrator**):

```powershell
Confirm-SecureBootUEFI
```

`False` albo błąd „not supported" (legacy BIOS) = możesz iść dalej.

### Krok 3 — włącz tryb testowy

PowerShell **jako administrator**:

```powershell
bcdedit /set testsigning on
```

➜ **RESTART.** Po nim w prawym dolnym rogu pulpitu pojawi się znak wodny
„Tryb testowy”.

> **Co to realnie kosztuje.** Maszyna zaczyna akceptować dowolny sterownik
> jądra podpisany certyfikatem, któremu ufa jej własny magazyn — a dopisać tam
> może każdy proces z uprawnieniami administratora. Znika jedna z warstw
> chroniących przed rootkitami. Odwracalne: patrz [sekcja 7](#7-cofnięcie-wszystkiego).

### Krok 4 — certyfikat i podpis

PowerShell **jako administrator** (elewacja jest po to, żeby od razu zaufać
certyfikatowi):

```powershell
cd F:\Projects\radio-voice\driver
.\tools\make-test-cert.ps1
.\tools\sign.ps1
```

Żaden z tych skryptów o nic nie pyta. Klucz prywatny zostaje w Twoim magazynie
certyfikatów, `signtool` sięga po niego po odcisku palca.

`sign.ps1` generuje katalog i podpisuje **oba** pliki — `.sys`, żeby jądro
załadowało obraz, i `.cat`, żeby PnP przyjął pakiet przy instalacji.

### Krok 5 — zainstaluj

PowerShell **jako administrator**:

```powershell
.\tools\install.ps1
```

Weryfikacja:

```powershell
Get-PnpDevice -FriendlyName '*RadioVoice*'
```

Powinny być dwa urządzenia w stanie `OK`. W panelu dźwięku:

- **Odtwarzanie** → `RadioVoice Output`
- **Nagrywanie** → `RadioVoice Microphone`

### Krok 6 — wskaż je w aplikacji

W RadioVoice ustaw **Output** na `RadioVoice Output` i kliknij **Restart audio**.

---

## 4. Konfiguracja w aplikacjach docelowych

Wszędzie ta sama zasada: wybierz jako **mikrofon** wyjściową stronę kabla.

| Droga | Co wybrać jako mikrofon |
|---|---|
| A (VB-CABLE) | `CABLE Output (VB-Audio Virtual Cable)` |
| B (własny sterownik) | `RadioVoice Microphone` |

- **Discord** — Ustawienia → Głos i wideo → Urządzenie wejściowe.
  **Wyłącz tam redukcję szumów i AGC** (Krisp, „Echo Cancellation”,
  „Automatic Gain Control”) — inaczej Discord przetworzy jeszcze raz sygnał,
  który już jest przetworzony, i pobije się z bramką.
- **OBS** — Źródło → Przechwytywanie wejścia audio.
- **Teams / Zoom / Meet** — ustawienia urządzeń, mikrofon.

---

## 5. Codzienne użycie

RadioVoice musi **działać w tle**, żeby dźwięk płynął — to on przetwarza i
podaje sygnał do kabla. Zamknięcie okna zatrzymuje tor.

- **Bypass all** — puszcza surowy mikrofon bez przetwarzania, do porównania A/B
- **Mute** — cisza na wyjściu
- ustawienia zapisują się same; zamknięcie okna zapisuje też stan wtyczek

Ustawianie bramki: gadaj normalnie i zjeżdżaj progiem w dół, aż wskaźnik
**OPEN/SHUT** przestanie migotać między słowami. `hysteresis` powiększ, jeśli
bramka nadal „klekocze”.

---

## 6. Gdy coś nie działa

Pierwszy przystanek zawsze: **przycisk `Log`** w prawym górnym rogu, albo
`%APPDATA%\RadioVoice\radiovoice.log`.

**Miernik INPUT stoi, mimo że mówisz**
Aplikacja pokaże ostrzeżenie, jeśli urządzenie jest otwarte, ale nic nie
dostarcza. Sprawdź, czy mikrofon nie jest wyciszony w ustawieniach dźwięku
Windows, i czy w Prywatności jest zgoda na dostęp aplikacji desktopowych do
mikrofonu.

**Nie ma na co ustawić Output**
Nie masz zainstalowanego żadnego kabla wirtualnego — [sekcja 3A](#3a-wirtualne-wyjście--droga-a-vb-cable)
albo [3B](#3b-wirtualne-wyjście--droga-b-własny-sterownik).

**`dropouts` rośnie**
Zwiększ **Processing block** (256 → 512). Jeśli `dsp` jest blisko 100%, łańcuch
nie mieści się w terminie — wyłącz najcięższą wtyczkę.

**`drift` przyklejony do ±5000 ppm**
Zegary wejścia i wyjścia rozjeżdżają się bardziej, niż resampler potrafi
nadrobić. Zdarza się przy dwóch urządzeniach czysto programowych. Spróbuj innej
pary urządzeń albo tej samej częstotliwości po obu stronach.

**Aplikacja nie startuje**
Log zapisuje ścieżkę wtyczki tuż przed jej załadowaniem. Jeśli wtyczka wywali
proces, przy kolejnym starcie trafi na czarną listę i aplikacja wstanie.
Czarną listę wyczyścisz w oknie „Add plugin”.

**Brak wtyczek na liście**
Skanowane są standardowe katalogi VST3. Wtyczki 32-bitowe z
`Program Files (x86)\Common Files\VST3` nie załadują się do 64-bitowego procesu
— log powie to wprost (błąd 193).

**Sterownik** — osobna lista objawów jest w
[`driver/README.md`](driver/README.md#gdy-coś-nie-działa).

---

## 7. Cofnięcie wszystkiego

### Sterownik

PowerShell **jako administrator**:

```powershell
cd F:\Projects\radio-voice\driver
.\tools\uninstall.ps1
bcdedit /set testsigning off
```

➜ **RESTART.** Potem włącz z powrotem Secure Boot w firmware.

Certyfikat testowy usuniesz tak:

```powershell
Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher, Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq 'CN=RadioVoice Test Signing' } | Remove-Item -Force
```

### Aplikacja

Nie instaluje się nigdzie — wystarczy skasować katalog `build` i
`%APPDATA%\RadioVoice`.

### Narzędzia

```powershell
winget uninstall Microsoft.WindowsWDK.10.0.26100
winget uninstall Microsoft.WindowsSDK.10.0.26100
winget uninstall Microsoft.VisualStudio.2022.BuildTools
```
