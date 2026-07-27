# RadioVoice Virtual Audio Cable — sterownik

Sterownik jądra tworzący parę endpointów audio połączonych wewnętrznie:

| Endpoint | Widoczny jako | Rola |
|---|---|---|
| `RadioVoice Output` | urządzenie odtwarzania | RadioVoice tu renderuje |
| `RadioVoice Microphone` | urządzenie nagrywania | Discord / OBS / Teams to wybierają |

Wszystko zapisane do pierwszego pojawia się na drugim. Funkcjonalny odpowiednik
VB-CABLE, bez zewnętrznej zależności.

---

## Stan

**Kompiluje się i linkuje czysto** (MSVC 19.44, WDK 10.0.26100, x64 Release i
Debug, obraz Native z entry point `DriverEntry`, bez ostrzeżeń poza
informacyjnym komunikatem z `stdunk.h` samego WDK).

**Nie był uruchomiony.** Załadowanie go wymaga trybu testowego podpisywania i
restartu maszyny — nie robiłem tego. Traktuj to jako kod, który przeszedł
kompilator, nie jako przetestowany sterownik. Pierwsze uruchomienie rób z
włączonym debuggerem jądra albo na maszynie wirtualnej; sekcja
[Gdy coś nie działa](#gdy-coś-nie-działa) wymienia miejsca, które najpewniej
będą wymagały poprawek.

---

## Wymagania

- Windows 10 wersja 2004 (build 19041) lub nowszy, x64
- Windows SDK + WDK w tej samej wersji
- Toolset MSVC x64

Na czystej maszynie:

```powershell
winget install --id Microsoft.WindowsSDK.10.0.26100 --exact
winget install --id Microsoft.WindowsWDK.10.0.26100 --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override `
  "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools"
```

Instalacje SDK i WDK są maszynowe, więc wyskoczy UAC.

---

## Budowanie

```powershell
cd driver
.\build.ps1 -Configuration Release
```

Wynik: `driver\build\Release\RadioVoiceAudio.sys` + `.inf`.

`build.ps1` woła `cl.exe` i `link.exe` bezpośrednio, zamiast używać projektu
`.vcxproj`. Projekt sterownika wymaga integracji WDK z Visual Studio, która
instaluje się jako VSIX i podpina wyłącznie do pełnego VS — nie do Build Tools.
Wywołanie kompilatora wprost usuwa to sprzężenie i sprawia, że wszystkie flagi
są widoczne w jednym pliku.

---

## Podpisywanie

64-bitowy Windows nie załaduje niepodpisanego sterownika jądra. Są dwie drogi.

### Droga produkcyjna

Certyfikat EV code-signing (~400–600 USD/rok), konto w Microsoft Partner Center,
przesłanie sterownika do Hardware Dev Center i odebranie podpisu Microsoftu.
Wtedy sterownik ładuje się wszędzie, bez zmian w konfiguracji maszyny.

### Droga lokalna (tryb testowy)

To, o co prosiłeś. Trzy kroki, wszystkie odwracalne.

#### 1. Wyłącz Secure Boot

W firmware (UEFI). Bez tego `bcdedit /set testsigning on` zamelduje sukces, ale
ustawienie nie zadziała — Secure Boot blokuje zmianę polityki podpisów.

Sprawdzenie stanu (PowerShell jako administrator):

```powershell
Confirm-SecureBootUEFI
```

#### 2. Włącz tryb testowy

PowerShell **jako administrator**, potem restart:

```powershell
bcdedit /set testsigning on
```

Po restarcie w prawym dolnym rogu pulpitu pojawi się znak wodny „Tryb testowy”.

> **Co to realnie oznacza.** Maszyna zaczyna akceptować dowolny sterownik jądra
> podpisany certyfikatem, któremu ufa jej własny magazyn — a do tego magazynu
> może dopisywać każdy proces z uprawnieniami administratora. Znika jedna z
> warstw chroniących przed rootkitami. Na maszynie deweloperskiej to
> akceptowalny kompromis; na maszynie, na której trzymasz coś wartościowego,
> przemyśl to jeszcze raz. Wyłączenie: `bcdedit /set testsigning off` + restart.

#### 3. Certyfikat i podpis

```powershell
cd driver
.\tools\make-test-cert.ps1          # elevated, żeby od razu zaufać certyfikatowi
.\tools\sign.ps1
```

`make-test-cert.ps1` tworzy certyfikat, eksportuje `.pfx` i `.cer`, i wstawia
`.cer` do `LocalMachine\Root` oraz `LocalMachine\TrustedPublisher`. Pierwszy
magazyn sprawia, że podpis daje się zweryfikować; drugi wycisza pytanie „Czy
chcesz zainstalować to oprogramowanie urządzenia?”.

`sign.ps1` generuje katalog (`Inf2Cat`) i podpisuje **oba** pliki:

- `.sys` — żeby jądro załadowało obraz. Bez tego: kod 577,
  `STATUS_INVALID_IMAGE_HASH`.
- `.cat` — żeby PnP przyjął pakiet przy instalacji.

Podpisanie tylko jednego daje mylący stan „w połowie działa”.

---

## Instalacja

```powershell
.\tools\install.ps1     # elevated
```

Dwa osobne kroki, które łatwo pomylić:

1. `pnputil /add-driver` wkłada pakiet do magazynu sterowników — czyni go
   *dostępnym*, ale niczego nie tworzy.
2. Urządzenie trzeba powołać do życia osobno. Nie ma sprzętu, który by je
   wyliczył, więc jest to urządzenie root-enumerated i `devcon` musi jawnie
   utworzyć węzeł o ID `root\RadioVoiceAudio`.

Pominięcie kroku 2 to najczęstszy powód, dla którego wirtualny sterownik
instaluje się „pomyślnie”, a żaden endpoint się nie pojawia.

Weryfikacja:

```powershell
Get-PnpDevice -FriendlyName '*RadioVoice*'
```

Potem w RadioVoice ustaw **Output** na `RadioVoice Output`, a w Discordzie
mikrofon na `RadioVoice Microphone`.

### Odinstalowanie

```powershell
.\tools\uninstall.ps1   # elevated
```

---

## Jak to działa

```
            aplikacja renderuje
                     │
                     ▼
   ┌─────────────────────────────────┐
   │  WaveRender  ──►  TopologyRender│  ──► endpoint "RadioVoice Output"
   └────────┬────────────────────────┘
            │  timer kopiuje bufor WaveRT ──► pierścień
            ▼
      ┌───────────┐
      │  40 ms    │   LoopbackBuffer
      │  pierścień│
      └───────────┘
            │  timer kopiuje pierścień ──► bufor WaveRT
            ▲
   ┌────────┴────────────────────────┐
   │ TopologyCapture ──► WaveCapture │  ──► endpoint "RadioVoice Microphone"
   └─────────────────────────────────┘
                     │
                     ▼
            aplikacja nagrywa
```

Cztery filtry: para wave + topology na każdy kierunek. Filtr topologii jest tym,
co w ogóle sprawia, że endpoint pojawia się w panelu dźwięku — budowniczy
endpointów szuka pinu o kategorii „głośnik” albo „mikrofon”. Sam filtr wave
byłby niewidoczny.

**Brak sprzętu oznacza brak DMA.** W WaveRT to silnik DMA przesuwa rejestr
pozycji; tutaj robi to timer wysokiej rozdzielczości, kopiując przy każdym tyknięciu
tyle bajtów, ile wynika z zadeklarowanego formatu. Pozycja liczona jest z zegara
przerwań, nie przez dodawanie stałej na tyknięcie — callbacki timera bywają
spóźnione, a pozycja akumulująca ten błąd odjechałaby od czasu rzeczywistego.

**Jeden format, celowo.** 48 kHz, 2 kanały, float32. Pętla to kopiowanie bajt w
bajt, więc gdyby strona render mogła otworzyć się jako 16-bit 44,1 kHz, a strona
capture jako float32 48 kHz, bajty przechodzące przez pierścień nie znaczyłyby
nic. Jeden format czyni tę niezgodność niereprezentowalną, zamiast czymś, co
pierścień musi wykrywać i konwertować. Silnik audio Windows i tak przelicza dla
klientów chcących czegokolwiek innego.

### Pliki

| Plik | Rola |
|---|---|
| `Common.h` | format, rozmiary buforów, indeksy pinów, nazwy podurządzeń |
| `Driver.cpp` | `DriverEntry`, `AddDevice`, wyładowanie |
| `Adapter.cpp` | `StartDevice` — tworzy 4 filtry i połączenia fizyczne |
| `Descriptors.cpp` | deskryptory filtrów, pinów, zakresy danych |
| `MinWaveRT.cpp` | miniport WaveRT i strumienie (oba kierunki) |
| `MinTopo.cpp` | miniport topologii |
| `LoopbackBuffer.cpp` | pierścień łączący render z capture |

---

## Gdy coś nie działa

Kod nie był uruchamiany, więc to lista miejsc, które sprawdziłbym najpierw.

**Sterownik się nie ładuje (kod 577)** — podpis `.sys`. Sprawdź, czy tryb
testowy faktycznie działa (znak wodny na pulpicie), i czy certyfikat jest w
`LocalMachine\Root`.

**Instaluje się, ale nie ma endpointów** — najczęściej rozjazd między nazwami
podurządzeń. Ciągi `RV_WAVE_RENDER_NAME` i pokrewne w `Common.h` muszą się
zgadzać co do znaku z `KSNAME_*` w INF. Rozjazd nie generuje żadnego błędu —
sterownik ładuje się i milczy.

Drugi kandydat: `PcRegisterPhysicalConnection`. Kierunek połączenia różni się
między render a capture i musi wskazywać zgodnie z przepływem sygnału, inaczej
budowniczy endpointów za nim nie pójdzie.

**Endpoint jest, ale cisza** — pętla timera w `MinWaveRT.cpp::OnTick`. Podejrzane
w kolejności: czy `SetState(KSSTATE_RUN)` w ogóle przychodzi, czy timer
wystartował, czy `m_bufferSize` jest sensowny.

**Trzaski albo dryf** — `RV_LOOPBACK_MS` (40 ms) i sposób liczenia pozycji.
Oba endpointy mają niezależne timery, więc pierścień musi absorbować ich
rozjazd.

Podgląd logów: DebugView z opcją „Capture Kernel” (makro `RV_LOG` woła
`DbgPrintEx`, aktywne tylko w buildzie Debug).

---

## Licencja

GPL-3.0-or-later, tak jak reszta projektu. Linkuje wyłącznie nagłówki i
biblioteki importu z WDK, objęte licencją Microsoftu i nieredystrybuowane tutaj.
