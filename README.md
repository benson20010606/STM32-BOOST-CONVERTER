# STM32-BOOST-CONVERTER
## 0814
### Progress

* PWM: TIM3 CH1, 100 kHz, 30% initial duty
* ADC: ADC1_IN0, TIM2 TRGO trigger at 5 kHz
* ADC conversion complete handled by interrupt
* Telemetry: TIM4 at 50 Hz
* UART TX changed to interrupt-driven mode
* ADC verified with fixed 10 kΩ divider and 10 kΩ potentiometer

### Next

* UART RX interrupt
* Vout feedback conversion
* PI controller
* Boost converter closed-loop test
