// Определяем общий пин, к которому подключена шина OneWire с несколькими датчиками
const uint8_t ONEWIRE_PIN = 2; // Все датчики сидят параллельно на контакте D2

// Уникальные 64-битные ROM-адреса датчиков (8 байт на каждый датчик).
// Внимание: замените эти значения на реальные адреса ваших датчиков!
uint8_t sensor1_address[8] = { 0x28, 0x78, 0xFA, 0x56, 0xB5, 0x01, 0x3C, 0xBD };
uint8_t sensor2_address[8] = { 0x28, 0x1B, 0x35, 0x56, 0xB5, 0x01, 0x3C, 0x6A };

// Переменные для реализации неблокирующего таймера (замена задержки delay)
unsigned long lastConversionTime = 0; // Время (в мс) последнего вызова команды Convert T
bool isConverting = false;            // Флаг: выполняется ли прямо сейчас замер температуры

// --- НИЗКОУРОВНЕВЫЕ ФУНКЦИИ ПРОТОКОЛА ONEWIRE ---

// 1. Импульс сброса (Reset) и проверка ответа от шины (Presence)
bool onewire_reset() {
  bool presence = false; // Переменная для хранения флага ответа от датчиков

  // Отключаем прерывания для защиты микросекундных таймингов
  noInterrupts();

  pinMode(ONEWIRE_PIN, OUTPUT);   // Конфигурируем пин как выход
  digitalWrite(ONEWIRE_PIN, LOW); // Притягиваем шину к 0V (начинаем импульс Reset)
  delayMicroseconds(480);         // Удерживаем LOW минимум 480 мкс по спецификации

  pinMode(ONEWIRE_PIN, INPUT);    // Переводим пин в режим входа (отпускаем шину к 5V)
  delayMicroseconds(70);          // Ждем 70 мкс перед считыванием ответа от устройств

  // Если хотя бы один датчик на шине ответил, он притянет шину к LOW
  presence = !digitalRead(ONEWIRE_PIN);
  
  interrupts();                   // Разрешаем прерывания

  delayMicroseconds(410);         // Дожидаемся завершения полного цикла Reset (480 мкс)
  return presence;                // true = устройства на шине обнаружены
}

// 2. Функция записи одного бита данных на шину
void onewire_write_bit(uint8_t bit) {
  noInterrupts();
  pinMode(ONEWIRE_PIN, OUTPUT);
  digitalWrite(ONEWIRE_PIN, LOW);

  if (bit) {
    // Передача логической '1'
    delayMicroseconds(6);
    pinMode(ONEWIRE_PIN, INPUT);
    interrupts();
    delayMicroseconds(64);
  } else {
    // Передача логического '0'
    delayMicroseconds(60);
    pinMode(ONEWIRE_PIN, INPUT);
    interrupts();
    delayMicroseconds(10);
  }
}

// 3. Функция чтения одного бита данных с шины
uint8_t onewire_read_bit() {
  uint8_t bit = 0;

  noInterrupts();
  pinMode(ONEWIRE_PIN, OUTPUT);
  digitalWrite(ONEWIRE_PIN, LOW);
  delayMicroseconds(3);

  pinMode(ONEWIRE_PIN, INPUT);
  delayMicroseconds(9);

  if (digitalRead(ONEWIRE_PIN)) {
    bit = 1;
  }
  interrupts();

  delayMicroseconds(55);
  return bit;
}

// 4. Последовательная отправка байта (8 бит) на шину, начиная с младшего бита (LSB)
void onewire_write_byte(uint8_t data) {
  for (uint8_t i = 0; i < 8; i++) {
    onewire_write_bit(data & 0x01); // Извлекаем младший бит
    data >>= 1;                      // Сдвигаем байт вправо
  }
}

// 5. Последовательное чтение байта (8 бит) с шины, начиная с младшего бита (LSB)
uint8_t onewire_read_byte() {
  uint8_t data = 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (onewire_read_bit()) {
      data |= (1 << i);             // Записываем полученный бит в соответствующую позицию
    }
  }
  return data;
}

// --- ФУНКЦИИ ВЕРХНЕГО УРУВНЯ ДЛЯ РАБОТЫ С АДРЕСАМИ ---

// Запуск измерения температуры СРАЗУ НА ВСЕХ датчиках шины
void ds18b20_start_all_conversions() {
  if (onewire_reset()) {
    onewire_write_byte(0xCC); // Команда Skip ROM (широковещательный запрос ко ВСЕМ устройствам)
    onewire_write_byte(0x44); // Команда Convert T (запуск замера температуры)
  }
}

// Чтение температуры с конкретного датчика по его 64-битному ROM-адресу
float ds18b20_read_temperature_by_address(const uint8_t* address) {
  if (!onewire_reset()) return -999.0; // Проверяем исправность шины

  onewire_write_byte(0x55); // Команда Match ROM (выбор конкретного устройства по адресу)
  
  // Передаем 8 байт уникального адреса нужного датчика
  for (uint8_t i = 0; i < 8; i++) {
    onewire_write_byte(address[i]);
  }

  onewire_write_byte(0xBE); // Команда Read Scratchpad (прочитать память выбранного датчика)

  // Читаем младший и старший байты температуры
  uint8_t lowByte = onewire_read_byte();
  uint8_t highByte = onewire_read_byte();

  int16_t rawTemperature = (highByte << 8) | lowByte;

  // Проверка на сбой/заводское значение по умолчанию (85.0 °C)
  if (rawTemperature == 0x0500) {
    return -999.0;
  }

  // Перевод сырого значения в градусы Цельсия (цена младшего разряда = 0.0625 °C)
  return rawTemperature * 0.0625;
}

// --- ИНИЦИАЛИЗАЦИЯ И ОСНОВНОЙ ЦИКЛ ---

void setup() {
  Serial.begin(9600);           // Настройка скорости Serial-порта
  pinMode(LED_BUILTIN, OUTPUT); // Пин встроенного светодиода
  
  Serial.println("=== Схема на колене ===");
  Serial.println("DS18B20 (2 датчика на одной шине по ROM-адресам) запущен!");

  // Отправляем общую команду на запуск первого измерения при старте
  ds18b20_start_all_conversions();
  
  lastConversionTime = millis(); // Запоминаем время запуска
  isConverting = true;           // Устанавливаем флаг замера
}

void loop() {
  // Неблокирующий таймер: выжидаем 800 мс без блокировки микроконтроллера
  if (isConverting && (millis() - lastConversionTime >= 800)) {
    // Поочередно считываем данные с датчиков по их уникальным адресам
    float temp1 = ds18b20_read_temperature_by_address(sensor1_address);
    float temp2 = ds18b20_read_temperature_by_address(sensor2_address);

    // Вывод результатов в одну строку с фирменной подписью канала
    Serial.print("[Схема на колене]  Sensor 1: ");
    if (temp1 != -999.0) {
      Serial.print(temp1, 1);   // Градусы с 1 знаком после запятой
      Serial.print(" °C");
    } else {
      Serial.print("Ошибка");  // Сбой при чтении первого датчика
    }

    Serial.print("  |  Sensor 2: ");
    if (temp2 != -999.0) {
      Serial.print(temp2, 1);   // Градусы с 1 знаком после запятой
      Serial.println(" °C");
    } else {
      Serial.println("Ошибка"); // Сбой при чтении второго датчика
    }

    // Запускаем новое одновременное измерение на всех датчиках
    ds18b20_start_all_conversions();
    
    lastConversionTime = millis(); // Обновляем метку времени
  }

  // Фоновая задача: мигаем встроенным светодиодом каждые 100 мс (доказывает отсутствие delay)
  static unsigned long ledTimer = 0;
  if (millis() - ledTimer >= 100) {
    ledTimer = millis();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}
