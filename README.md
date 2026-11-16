# PresenceSimulator dla ESP32-C3 SuperMini

Port koncepcji **[SensorsIot/PresenceSimulator](https://github.com/SensorsIot/PresenceSimulator)** Andreasa Spiessa na ESP32-C3. Kod przeznaczony dla Arduino IDE i PlatformIO. Oryginalny projekt (2016) jest opublikowany na licencji MIT; licencja i wskazanie autora pozostają w tym projekcie.

> **Status:** przygotowany wariant źródłowy, **niezweryfikowany na fizycznym ESP32-C3**. Nie wykonano kompilacji na docelowym pakiecie Arduino-ESP32 ani testów elektrycznych na taśmie. Przed pozostawieniem urządzenia bez nadzoru przetestuj działanie na stole.

## Co zmieniono względem ESP8266

- Obsługa `WiFi.h`, `WebServer.h`, `Preferences` i standardowego NTP z Arduino-ESP32; bez `ESP8266WiFi`, `ESP8266httpUpdate`, RTC ESP8266, `RemoteDebug`, `SNTPtime`, `credentials.h`.
- Zachód słońca liczony **lokalnie**, na podstawie współrzędnych. Nie potrzeba konta ani klucza OpenWeatherMap. Zastosowany algorytm astronomiczny jest przybliżony (zwykle dokładność rzędu kilku minut dla szerokości geograficznych Polski).
- FakeTV jest **nieblokujący**; zmienia kolory bez zatrzymywania serwera WWW.
- Zamiast historycznego pliku `data.h` (~162 kB zarejestrowanych kolorów) zastosowano małą paletę scen i losowe przejścia. Efekt ma podobny cel, ale **nie jest wierną kopią sekwencji wizualnej oryginału**.
- Usunięto stare OTA wykonywane przez zewnętrzny serwer oraz telemetrię SparkFun; w oryginalnych plikach znajdowały się jawne klucze. Nie przeniesiono ich do portu.
- Można ustawić liczbę NeoPixeli, jasność, koordynaty geograficzne, godzinę zakończenia oraz tryby AUTO/ON/OFF z WWW.
- Ustawienia Wi-Fi zapisywane w pamięci NVS, nie w kodzie. Hasło Wi-Fi nie jest pokazywane na stronie ustawień.

## Podłączenie NeoPixel WS2812B/WS2813 (5 V)

| ESP32-C3 SuperMini | Dokąd | Uwagi |
| --- | --- | --- |
| GPIO4 | `1A` układu **74AHCT125** | Sygnał danych, 3,3 V |
| GND | Masa zasilacza 5 V / `GND` LED / GND układu AHCT | **Wspólna masa obowiązkowa** |
| GPIO5 | Wejście zewnętrznego sterownika lampy (opcjonalnie) | Stan wysoki = światło włączone; *nie* steruj bezpośrednio cewką ani 230 V |
| GPIO3 | Przycisk do GND (opcjonalnie) | Trzymając podczas startu, wymusisz punkt AP konfiguracyjny |

Zalecany układ transmisji: `ESP32 GPIO4 -> 74AHCT125 wejście 1A (pin 2) -> wyjście 1Y (pin 3) -> rezystor 330–470 Ω -> DIN pierwszego NeoPixela`. W przypadku DIP-14: `1OE` (pin 1) do GND, `GND` (pin 7) do masy, `VCC` (pin 14) do +5 V. Pozostałe niewykorzystane wejścia układu nie powinny pozostawać przypadkowo niepodłączone.

**Taśmę zasila osobny zasilacz 5 V**. Połącz `+5 V` z `5V`/`V+` taśmy, `GND` z `GND` taśmy, a pomiędzy +5 i GND przy pierwszej diodzie umieść kondensator 1000 µF (minimum 6,3 V, zalecane 10 V). Połącz GND z GND ESP32. Nie podłączaj `5 V` taśmy do pinu `3V3` ESP32; nie zasilaj 60 diod z wyjścia 5 V niewielkiej płytki bez sprawdzenia dopuszczalnego prądu. Nie łącz zewnętrznego +5 V na sztywno z pinem 5 V ESP32, gdy ESP32 jest podłączony przez USB, bez sprawdzenia układu separacji zasilania.

### Ile amperów?

Prąd maksymalny dla typowych 5 V RGB WS2812B bywa szacowany na **60 mA/diodę przy pełnej bieli**. Dla 60 diod daje to do 3,6 A. Używaj zasilacza z zapasem (np. 5 V / 4 A przy pełnej jasności). Ten port startuje z jasnością 72/255 i używa kolorów stonowanych, więc typowe pobory będą niższe, ale nie zastępuje to poprawnego doboru zasilania, przewodów i zabezpieczeń. Dla taśm dłuższych niż kilka dziesiątek pikseli potrzebny może być wtrysk zasilania w kilku punktach.

Schemat: [wiring-ESP32-C3-NeoPixel.svg](wiring-ESP32-C3-NeoPixel.svg).

## Wgrywanie z Arduino IDE

1. Zainstaluj `esp32 by Espressif Systems` w **Boards Manager** (zalecany Arduino-ESP32 3.x).
2. W **Library Manager** zainstaluj `Adafruit NeoPixel` (Adafruit).
3. Otwórz `PresenceSimulator_C3/PresenceSimulator_C3.ino`.
4. Wybierz płytkę `ESP32C3 Dev Module` (dla SuperMini) i właściwy port COM. Jeśli to płytka z natywnym USB, włącz `USB CDC On Boot` (nazwa ustawienia zależy od pakietu).
5. Zmień w szkicu `AP_PASSWORD` na własne hasło (min. 8 znaków). Dostosuj piny, jeśli masz inny model ESP32-C3.
6. Wgraj program. Otwórz monitor portu szeregowego `115200`.
7. Przy pierwszym uruchomieniu połącz się z Wi-Fi `PresenceC3-Setup` (hasło zgodne z `AP_PASSWORD`), otwórz `http://192.168.4.1`, wpisz Wi-Fi, szerokość i długość geograficzną i zapisz.
8. Urządzenie uruchomi się ponownie. W LAN otwórz adres IP wypisany w konsoli lub `http://presence-c3.local` (jeśli mDNS jest obsługiwane w sieci).
9. Test: naciśnij `WŁĄCZ` — NeoPixele powinny symulować TV, a GPIO5 być w stanie HIGH. `WYŁĄCZ` gasi oba wyjścia; `AUTO` wraca do harmonogramu.

Jeśli nie uda się połączyć z zapisanym Wi-Fi, po ok. 15 sekundach uruchomi się AP konfiguracyjny. Można go też wymusić przytrzymując przycisk podłączony do GPIO3 podczas resetu/zasilania.

## Wgrywanie z PlatformIO

Otwórz katalog główny w VS Code + PlatformIO. Plik `platformio.ini` zawiera środowisko `esp32c3`, a `src/main.cpp` korzysta z tego samego kodu co Arduino IDE. Skompiluj `Build`, następnie `Upload` i `Monitor` (115200).

## Harmonogram i panel WWW

- Automatycznie: od **60 minut przed zachodem słońca** do **22:00 czasu lokalnego** (ustawienia można zmienić).
- Gdy zegar nie ma jeszcze aktualnego czasu NTP, tryb AUTO pozostawia wyjścia wyłączone. Ręczne WŁĄCZ działa także bez NTP.
- Podczas harmonogramu zmieniane są losowo 3 tryby: TV, światło, TV+światło (co 7–15 minut).
- Kolory TV przechodzą w nową scenę co 0,25–2,5 sekundy, z okazjonalnym natychmiastowym cięciem sceny.
- Przykładowe współrzędne w kodzie dotyczą **Warszawy**. Ustaw *swoją* lokalizację; nie zakładaj, że wartość domyślna jest właściwa.
- Domyślny `timezone`: `CET-1CEST,M3.5.0/2,M10.5.0/3`, czyli Polska z automatyczną zmianą czasu. W innych strefach wpisz właściwą wartość POSIX TZ.

## Bezpieczeństwo

Panel WWW **nie ma logowania** — projekt zakłada zaufaną sieć LAN; nie wystawiaj portu 80 do Internetu i najlepiej wydziel urządzenia IoT w osobnym VLAN/SSID. Punkt AP ma hasło, ale `AP_PASSWORD` w źródle należy zmienić przed wdrożeniem. Do sterowania lampą sieciową potrzebny jest odpowiednio dobrany moduł z izolacją, dopuszczeniami i bezpiecznym montażem; **GPIO5 to wyłącznie sygnał logiczny 3,3 V**, nie wyjście mocy.

## Ograniczenia

- Nowy kod nie przechodził w tym środowisku pełnej kompilacji z pakietem Arduino-ESP32 ani testów na rzeczywistej płytce; wymaga weryfikacji.
- Dla szerokości bliskich kołom podbiegunowym i w pobliżu międzynarodowej linii zmiany daty algorytm zachodu słońca wymaga dodatkowego dopracowania. Celem jest praca w Polsce/Europie.
- Współrzędne, liczba LED i jasność zapisują się do NVS; tryb manualny nie jest zapamiętywany po restarcie.
- Współpraca z WS2812B 5 V najpewniejsza przy 74AHCT125/74HCT245; podawanie 3,3 V bezpośrednio na DIN może działać, ale nie jest gwarantowane.

## Źródła i licencja

- [SensorsIot/PresenceSimulator](https://github.com/SensorsIot/PresenceSimulator) — oryginał, Andreas Spiess, MIT.
- [Espressif: ESP32-C3 GPIO/strapping](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/schematic-checklist.html).
- [Adafruit NeoPixel Überguide: Best Practices](https://learn.adafruit.com/adafruit-neopixel-uberguide/best-practices).