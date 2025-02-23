#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "inc/ssd1306.h"
#include "inc/font.h"

// Configurações do I2C e do Display
#define I2C_PORT      i2c1
#define I2C_SDA       14
#define I2C_SCL       15
#define OLED_ADDR     0x3C

// Definições dos LEDs
#define LED_RED       13  // LED vermelho (PWM) – controlado pelo eixo X
#define LED_GREEN     11  // LED verde (digital) – usado para indicar modo
#define LED_BLUE      12  // LED azul (PWM) – controlado pelo eixo Y

// Definições dos botões e do joystick
#define JOYSTICK_X_PIN 26  // ADC para eixo X
#define JOYSTICK_Y_PIN 27  // ADC para eixo Y
#define JOYSTICK_PB   22   // Botão integrado do joystick
#define BTN_A         5    // Botão A para alternar PWM
#define BTN_B         6    // Botão B para resetar o jogo

// Configurações do display
#define WIDTH         128
#define HEIGHT        64

// Configuração do labirinto (maze) – grade de 16 colunas x 8 linhas, cada célula de 8x8 pixels
#define MAZE_COLS 16
#define MAZE_ROWS 8
#define CELL_SIZE 8
#define GOAL_ROW 6
#define GOAL_COL 14
   

// Exemplo de labirinto: 1 = parede, 0 = caminho livre
uint8_t maze[MAZE_ROWS][MAZE_COLS] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,1,0,1,0,1,1,1,0,1},
    {1,0,1,0,0,0,0,1,0,0,0,1,0,1,0,1},
    {1,0,1,1,1,1,0,1,1,1,0,1,0,1,0,1},
    {1,0,0,0,0,1,0,0,0,1,0,0,0,1,0,1},
    {1,1,1,1,0,1,1,1,0,1,1,1,0,1,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

// Variáveis globais
ssd1306_t ssd;

volatile bool pwm_enabled = true;   // Ativação dos LEDs PWM (vermelho e azul)
volatile bool led_green_state = false; // Estado do LED verde
volatile int border_style = 0;         // Estilo da borda do display (0: simples, 1: tracejada, 2: sem borda)

// Variáveis de debounce e estado dos botões
volatile uint32_t last_press_btnA = 0;
volatile uint32_t last_press_joy = 0;
volatile bool btnA_busy = false;
volatile bool btnJoy_busy = false;
volatile uint32_t last_press_btnB = 0;
volatile bool btnB_busy = false;

// Variáveis PWM
uint slice_red, slice_blue;

// Variáveis do jogador no labirinto (posição na grade)
volatile int player_col = 1; // Coluna inicial
volatile int player_row = 1; // Linha inicial
volatile bool goal_reached = false;

// Prototipação de funções
void setup(void);
void update_pwm(uint16_t adc_x, uint16_t adc_y);
void update_player_position(uint16_t adc_x, uint16_t adc_y);
void update_display(void);
void debounce_button(uint gpio, volatile uint32_t *last_press, void (*callback)(void));
static void btn_a_callback(uint gpio, uint32_t events);
static void joy_button_callback(uint gpio, uint32_t events);
void toggle_pwm_action(void);
void toggle_joy_action(void);
void check_goal(ssd1306_t *ssd, int player_col, int player_row);
static void btn_b_callback(uint gpio, uint32_t events);
void reset_game(void);
static void gpio_global_callback();

int main(void) {
    stdio_init_all();
    setup();

    // Inicializa o ADC para os eixos do joystick
    adc_init();
    adc_gpio_init(JOYSTICK_X_PIN);
    adc_gpio_init(JOYSTICK_Y_PIN);

    uint16_t adc_x, adc_y;
    while (true) {

        if (!goal_reached) {
            adc_select_input(0);
            adc_x = adc_read();
            adc_select_input(1);
            adc_y = adc_read();
    
            update_pwm(adc_x, adc_y);
            update_player_position(adc_x, adc_y);
            update_display();
            check_goal(&ssd, player_col, player_row);
        }
        sleep_ms(50);
    }
    return 0;
}

void setup(void) {
    // Inicializa o I2C e o display SSD1306
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, OLED_ADDR, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);

    // Configuração dos LEDs
    // LED_RED e LED_BLUE: PWM
    gpio_init(LED_RED);
    gpio_set_function(LED_RED, GPIO_FUNC_PWM);
    gpio_init(LED_BLUE);
    gpio_set_function(LED_BLUE, GPIO_FUNC_PWM);
    // LED_GREEN: saída digital
    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_put(LED_GREEN, led_green_state);

    // Configura PWM para LED_RED e LED_BLUE
    slice_red = pwm_gpio_to_slice_num(LED_RED);
    slice_blue = pwm_gpio_to_slice_num(LED_BLUE);
    pwm_set_wrap(slice_red, 4095);
    pwm_set_wrap(slice_blue, 4095);
    pwm_set_chan_level(slice_red, pwm_gpio_to_channel(LED_RED), 0);
    pwm_set_chan_level(slice_blue, pwm_gpio_to_channel(LED_BLUE), 0);
    pwm_set_enabled(slice_red, true);
    pwm_set_enabled(slice_blue, true);

    // Botão do Joystick (para alternar LED verde e borda) – GPIO 22
    gpio_init(JOYSTICK_PB);
    gpio_set_dir(JOYSTICK_PB, GPIO_IN);
    gpio_pull_up(JOYSTICK_PB);
    // Habilita interrupção em ambas as bordas para detectar press e release
    gpio_set_irq_enabled_with_callback(JOYSTICK_PB, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_global_callback);

    // Botão A (para alternar a ativação dos LEDs PWM) – GPIO 5
    gpio_init(BTN_A);
    gpio_set_dir(BTN_A, GPIO_IN);
    gpio_pull_up(BTN_A);
    gpio_set_irq_enabled_with_callback(BTN_A, GPIO_IRQ_EDGE_FALL, true, &gpio_global_callback);

    // Botão B (para resetar o jogo) - GPIO 6
    gpio_init(BTN_B);
    gpio_set_dir(BTN_B, GPIO_IN);
    gpio_pull_up(BTN_B);
    gpio_set_irq_enabled_with_callback(BTN_B, GPIO_IRQ_EDGE_FALL, true, &gpio_global_callback);

}

// Atualiza os duty cycles dos LEDs PWM conforme a variação do joystick
void update_pwm(uint16_t adc_x, uint16_t adc_y) {
    if (pwm_enabled) {
        const int threshold = 100;  // Tolerância perto do centro (2048)
        int delta_x = abs((int)adc_x - 2048);
        int delta_y = abs((int)adc_y - 2048);
        uint16_t duty_red, duty_blue;
        
        // LED Vermelho (eixo X)
        if (delta_x < threshold) {
            duty_red = 0;
        } else {
            duty_red = (delta_x * 4095) / 2048;
            if (duty_red > 4095) duty_red = 4095;
        }
        
        // LED Azul (eixo Y)
        if (delta_y < threshold) {
            duty_blue = 0;
        } else {
            duty_blue = (delta_y * 4095) / 2048;
            if (duty_blue > 4095) duty_blue = 4095;
        }
        
        pwm_set_chan_level(slice_red, pwm_gpio_to_channel(LED_RED), duty_red);
        pwm_set_chan_level(slice_blue, pwm_gpio_to_channel(LED_BLUE), duty_blue);
    } else {
        pwm_set_chan_level(slice_red, pwm_gpio_to_channel(LED_RED), 0);
        pwm_set_chan_level(slice_blue, pwm_gpio_to_channel(LED_BLUE), 0);
    }
}

// Atualiza a posição do jogador no labirinto baseado nos valores ADC do joystick
void update_player_position(uint16_t adc_x, uint16_t adc_y) {
    const int move_threshold = 300; // Limite para detectar movimento
    int center = 2048;
    int moveX = 0, moveY = 0;
    if (adc_x < center - move_threshold) {
        moveX = -1; // mover para a esquerda
    } else if (adc_x > center + move_threshold) {
        moveX = 1;  // mover para a direita
    }
    if (adc_y < center - move_threshold) {
        moveY = -1; // mover para cima
    } else if (adc_y > center + move_threshold) {
        moveY = 1;  // mover para baixo
    }
    int new_col = player_col + moveX;
    int new_row = player_row + moveY;
    // Verifica limites e colisões: célula livre é 0
    if (new_col >= 0 && new_col < MAZE_COLS &&
        new_row >= 0 && new_row < MAZE_ROWS &&
        maze[new_row][new_col] == 0) {
        player_col = new_col;
        player_row = new_row;
    }
}

// Atualiza o display: desenha o labirinto, o jogador e a borda
void update_display(void) {
    ssd1306_fill(&ssd, false);
    // Desenha o labirinto: percorre as linhas e colunas
    for (int row = 0; row < MAZE_ROWS; row++) {
        for (int col = 0; col < MAZE_COLS; col++) {
            if (maze[row][col] == 1) { // parede
                // O primeiro parâmetro é a coordenada Y e o segundo a coordenada X
                ssd1306_fill_rect(&ssd, row * CELL_SIZE, col * CELL_SIZE, CELL_SIZE, CELL_SIZE, true);
            }
        }
    }
    // Desenha o jogador (quadrado de 8x8) na posição do jogador
    int player_x = player_col * CELL_SIZE;
    int player_y = player_row * CELL_SIZE;
    ssd1306_rect(&ssd, player_y, player_x, CELL_SIZE, CELL_SIZE, true, true);
    
    // Desenha a borda conforme o border_style
    if (border_style == 0) {
        // Borda simples
        ssd1306_rect(&ssd, 0, 0, WIDTH, HEIGHT, true, false);
    } else if (border_style == 1) {
        // Borda tracejada
        for (int i = 0; i < WIDTH; i += 4) {
            ssd1306_pixel(&ssd, i, 0, true);
            ssd1306_pixel(&ssd, i, HEIGHT - 1, true);
        }
        for (int i = 0; i < HEIGHT; i += 4) {
            ssd1306_pixel(&ssd, 0, i, true);
            ssd1306_pixel(&ssd, WIDTH - 1, i, true);
        }
    }
    // Se border_style == 2, não desenha borda

    ssd1306_send_data(&ssd);
}

// Função de debounce
void debounce_button(uint gpio, volatile uint32_t *last_press, void (*callback)(void)) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - *last_press > 200) {  // 200 ms debounce
        *last_press = now;
        if (gpio_get(gpio) == 0) { // Confirma que o botão está pressionado
            callback();
        }
    }
}

// Ação para o botão A: alterna a ativação dos LEDs PWM
void toggle_pwm_action(void) {
    pwm_enabled = !pwm_enabled;
    printf("PWM %s\n", pwm_enabled ? "ATIVADO" : "DESATIVADO");
    fflush(stdout);
    ssd1306_fill(&ssd, false);
    ssd1306_draw_string(&ssd, pwm_enabled ? "PWM ON" : "PWM OFF", 20, 10);
    ssd1306_send_data(&ssd);
}

// Ação para o botão do joystick: alterna o LED verde e o estilo da borda
void toggle_joy_action(void) {
    led_green_state = !led_green_state;
    gpio_put(LED_GREEN, led_green_state);
    border_style = (border_style + 1) % 3;  // Cicla entre 0, 1 e 2
    printf("LED Verde %s, Border style %d\n", led_green_state ? "Ligado" : "Desligado", border_style);
    fflush(stdout);
}

// Callback para o Botão A (GPIO 5)
static void btn_a_callback(uint gpio, uint32_t events) {
    if (!btnA_busy) {
        btnA_busy = true;
        printf("BTN_A IRQ acionado\n");
        fflush(stdout);
        debounce_button(BTN_A, &last_press_btnA, toggle_pwm_action);
        btnA_busy = false;
    }
}

// Callback para o Botão do Joystick (GPIO 22)
static void joy_button_callback(uint gpio, uint32_t events) {
    bool current_state = (gpio_get(gpio) == 0);  // 0 = pressionado
    if (current_state && !btnJoy_busy) {
        btnJoy_busy = true;
        printf("Joystick botão IRQ acionado\n");
        fflush(stdout);
        debounce_button(JOYSTICK_PB, &last_press_joy, toggle_joy_action);
    } else if (!current_state) {
        btnJoy_busy = false;
    }
}

// Preenche um retângulo (definido por top, left, width e height) com o valor 'value'.
void ssd1306_fill_rect(ssd1306_t *ssd, uint8_t top, uint8_t left, uint8_t width, uint8_t height, bool value) {
    for (uint8_t y = top; y < top + height; y++) {
        for (uint8_t x = left; x < left + width; x++) {
            ssd1306_pixel(ssd, x, y, value);
        }
    }
}

void check_goal(ssd1306_t *ssd, int player_col, int player_row) {
    if (!goal_reached && player_col == GOAL_COL && player_row == GOAL_ROW) {
        goal_reached = true;
        // Limpa o display
        ssd1306_fill(ssd, false);
        // Exibe a mensagem final
        ssd1306_draw_string(ssd, "PARABENS!", 20, 20);
        ssd1306_draw_string(ssd, "LABIRINTO", 20, 32);
        ssd1306_draw_string(ssd, "CONCLUIDO!", 20, 44);
        ssd1306_send_data(ssd);
    }
}

void reset_game(void) {
    // Reinicializa a posição do jogador para a célula inicial
    player_col = 1;
    player_row = 1;
    // Reinicia o estado do jogo
    goal_reached = false;
    // Se necessário, reativa o PWM
    pwm_enabled = true;
    printf("Jogo reiniciado!\n");
    fflush(stdout);
    // Atualiza o display para mostrar o labirinto com o jogador na posição inicial
    update_display();
}

static void btn_b_callback(uint gpio, uint32_t events) {
    if (!btnB_busy) {
        btnB_busy = true;
        printf("BTN_B IRQ acionado - Reset\n");
        fflush(stdout);
        debounce_button(BTN_B, &last_press_btnB, reset_game);
        btnB_busy = false;
    }
}

static void gpio_global_callback(uint gpio, uint32_t events) {
    // Verifica qual pino gerou o evento
    if (gpio == JOYSTICK_PB) {
        // Chama o callback específico para o botão do joystick
        joy_button_callback(gpio, events);
    } else if (gpio == BTN_A) {
        // Chama o callback específico para o botão A
        btn_a_callback(gpio, events);
    }
    else if (gpio == BTN_B) {
        // Chama o callback específico para o botão B
        btn_b_callback(gpio, events);
    }
}