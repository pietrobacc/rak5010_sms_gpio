# =============================================================================
#  CMakeLists.txt - Definizione del progetto Zephyr
#  RAK5010-M + BG95-M3 SMS->GPIO Controller
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

# Individua l'SDK Zephyr tramite la variabile d'ambiente ZEPHYR_BASE,
# impostata automaticamente dall'ambiente nRF Connect in VS Code.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(rak5010_sms_gpio VERSION 1.1.0)

# Sorgenti dell'applicazione:
#   main.c       - Loop principale, dispatcher comandi SMS
#   gpio_ctrl.c  - Gestione GPIO (LED, PWRKEY BG95, output, ingresso IN4)
#   modem.c      - Comunicazione AT con BG95-M3 via UART0
#   sms.c        - Invio, ricezione e parsing SMS
#   sequence.c   - Macchina a stati della sequenza di avvio (thread dedicato)
#   auth.c       - Gestione autenticazione numeri telefonici
target_sources(app PRIVATE
    src/main.c
    src/gpio_ctrl.c
    src/modem.c
    src/sms.c
    src/sequence.c
    src/auth.c
    src/wdt.c
)

# Aggiunge src/ al path degli include in modo che i file .h siano
# trovati con #include "nome.h" senza specificare il percorso completo.
target_include_directories(app PRIVATE src)
