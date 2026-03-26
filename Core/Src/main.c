/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint32_t pedestrian_count = 0; // 人流计数变量
uint32_t last_interrupt_time = 0; // 上次中断时间，用于防抖
#define DEBOUNCE_TIME 200 // 防抖时间，单位ms

uint8_t hc_sr505_count = 0; // HC_SR505连续高电平计数
#define HC_SR505_THRESHOLD 5 // 连续高电平阈值
uint8_t hc_sr505_valid = 0; // HC_SR505有效标志

uint16_t threshold = 50; // 计数人数阈值，默认50人
uint8_t setting_mode = 0; // 设置模式标志，0-正常模式，1-设置模式

// 蜂鸣器和LED控制变量
uint8_t alarm_active = 0; // 报警激活标志
uint32_t alarm_start_time = 0; // 报警开始时间
#define ALARM_DURATION 5000 // 报警持续时间，单位ms
uint8_t alarm_triggered = 0; // 报警已触发标志，0-未触发，1-已触发

// 蓝牙连接状态
uint8_t ble_connected = 0; // 蓝牙连接状态，0-未连接，1-已连接

// 串口2接收缓冲区
uint8_t uart2_rx_buffer[100]; // 接收缓冲区
uint8_t uart2_rx_index = 0; // 接收索引
#define UART2_BUFFER_SIZE 100 // 缓冲区大小

// FLASH相关定义
#define FLASH_USER_START_ADDR   ((uint32_t)0x0801FC00) // FLASH用户存储区起始地址
#define FLASH_USER_END_ADDR     ((uint32_t)0x08020000) // FLASH用户存储区结束地址
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void FLASH_WriteThreshold(uint16_t value);
uint16_t FLASH_ReadThreshold(void);
void EnterSettingMode(void);
void ExitSettingMode(void);
void UpdateThresholdDisplay(void);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);
void CheckBLEStatus(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  写入阈值到FLASH
  * @param  value: 要写入的阈值
  * @retval None
  */
void FLASH_WriteThreshold(uint16_t value)
{
  HAL_FLASH_Unlock();
  
  // 擦除FLASH扇区
  FLASH_EraseInitTypeDef EraseInitStruct;
  EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
  EraseInitStruct.PageAddress = FLASH_USER_START_ADDR;
  EraseInitStruct.NbPages = 1;
  
  uint32_t PageError = 0;
  HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);
  
  // 写入阈值
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, FLASH_USER_START_ADDR, value);
  
  HAL_FLASH_Lock();
}

/**
  * @brief  从FLASH读取阈值
  * @retval 读取的阈值
  */
uint16_t FLASH_ReadThreshold(void)
{
  return *(__IO uint16_t*)FLASH_USER_START_ADDR;
}

/**
  * @brief  进入设置模式
  * @retval None
  */
void EnterSettingMode(void)
{
  setting_mode = 1;
  UpdateThresholdDisplay();
}

/**
  * @brief  退出设置模式
  * @retval None
  */
void ExitSettingMode(void)
{
  setting_mode = 0;
  // 保存阈值到FLASH
  FLASH_WriteThreshold(threshold);
  // 显示正常模式界面
  OLED_Clear();
  OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
  OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
  OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
  // 显示蓝牙连接状态
  if(ble_connected)
  {
    OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
  }
  else
  {
    OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
  }
  OLED_Refresh();
}

/**
  * @brief  更新阈值显示
  * @retval None
  */
void UpdateThresholdDisplay(void)
{
  OLED_Clear();
  OLED_ShowString(0, 0, (uint8_t*)"Setting Threshold", 8, 1);
  OLED_ShowString(0, 8, (uint8_t*)"Threshold:", 8, 1);
  OLED_ShowNum(56, 8, threshold, 3, 8, 1);
  OLED_ShowString(80, 8, (uint8_t*)"people", 8, 1);
  OLED_ShowString(0, 16, (uint8_t*)"KEY2: -10", 8, 1);
  OLED_ShowString(0, 24, (uint8_t*)"KEY3: +10", 8, 1);
  OLED_Refresh();
}

/**
  * @brief  UART Receive Completed Callback
  * @param  huart: UART handle
  * @retval None
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if(huart == &huart2)
  {
    // 读取接收到的数据
    static uint8_t rx_data;
    
    // 存储到缓冲区
    if(uart2_rx_index < UART2_BUFFER_SIZE - 1)
    {
      uart2_rx_buffer[uart2_rx_index++] = rx_data;
      uart2_rx_buffer[uart2_rx_index] = '\0'; // 确保字符串结束
    }
    else
    {
      // 缓冲区满，重置索引
      uart2_rx_index = 0;
    }
    
    // 检查是否接收到换行符或回车符，表示消息结束
    if(rx_data == '\n' || rx_data == '\r' || uart2_rx_index >= UART2_BUFFER_SIZE - 1)
    {
      // 检查蓝牙连接状态
      CheckBLEStatus();
    }
    
    // 重新启动接收中断
    HAL_UART_Receive_IT(&huart2, &rx_data, 1);
  }
}

/**
  * @brief  检查蓝牙连接状态并显示串口数据
  * @retval None
  */
void CheckBLEStatus(void)
{
  // 检查是否包含"CONNECT OK"
  if(strstr((char*)uart2_rx_buffer, "CONNECT OK") != NULL)
  {
    ble_connected = 1;
    // 如果不在设置模式，更新显示
    if(!setting_mode)
    {
      OLED_Clear();
      OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
      OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
      OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
      OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
//      OLED_ShowString(0, 16, (uint8_t*)"Rx:", 8, 1);
//      OLED_ShowString(24, 16, uart2_rx_buffer, 8, 1);
      OLED_Refresh();
    }
    // 清空缓冲区
    uart2_rx_index = 0;
    memset(uart2_rx_buffer, 0, UART2_BUFFER_SIZE);
  }
  // 检查是否包含"DISCONNECT"
  else if(strstr((char*)uart2_rx_buffer, "DISCONNECT") != NULL)
  {
    ble_connected = 0;
    // 如果不在设置模式，更新显示
    if(!setting_mode)
    {
      OLED_Clear();
      OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
      OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
      OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
      OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
      OLED_ShowString(0, 16, (uint8_t*)"Rx:", 8, 1);
      OLED_ShowString(24, 16, uart2_rx_buffer, 8, 1);
      OLED_Refresh();
    }
    // 清空缓冲区
    uart2_rx_index = 0;
    memset(uart2_rx_buffer, 0, UART2_BUFFER_SIZE);
  }
  // 检查是否包含"clear"命令
  else if(strstr((char*)uart2_rx_buffer, "clear") != NULL)
  {
    // 清理检测到的人数
    pedestrian_count = 0;
    alarm_triggered = 0;
    // 如果不在设置模式，更新显示
    if(!setting_mode)
    {
      OLED_Clear();
      OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
      OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
      OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
      // 显示蓝牙连接状态
      if(ble_connected)
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
      }
      else
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
      }
      OLED_ShowString(0, 16, (uint8_t*)"Clear count done", 8, 1);
      OLED_Refresh();
    }
    // 清空缓冲区
    uart2_rx_index = 0;
    memset(uart2_rx_buffer, 0, UART2_BUFFER_SIZE);
  }
  // 检查是否包含"alert"命令
  else if(strstr((char*)uart2_rx_buffer, "alert") != NULL)
  {
    // 手动触发报警
    alarm_active = 1;
    alarm_start_time = HAL_GetTick();
    alarm_triggered = 1;
    // 控制蜂鸣器和LED
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    // 如果不在设置模式，更新显示
    if(!setting_mode)
    {
      OLED_Clear();
      OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
      OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
      OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
      // 显示蓝牙连接状态
      if(ble_connected)
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
      }
      else
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
      }
      OLED_ShowString(0, 16, (uint8_t*)"Manual alarm", 8, 1);
      OLED_Refresh();
    }
    // 清空缓冲区
    uart2_rx_index = 0;
    memset(uart2_rx_buffer, 0, UART2_BUFFER_SIZE);
  }
  // 其他数据，直接显示
  else if(uart2_rx_index > 0)
  {
    // 如果不在设置模式，更新显示
    if(!setting_mode)
    {
      OLED_Clear();
      OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
      OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
      OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
      // 显示蓝牙连接状态
      if(ble_connected)
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
      }
      else
      {
        OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
      }
      // 显示接收到的数据
      OLED_ShowString(0, 16, (uint8_t*)"Rx:", 8, 1);
      OLED_ShowString(24, 16, uart2_rx_buffer, 8, 1);
      OLED_Refresh();
    }
    // 清空缓冲区
    uart2_rx_index = 0;
    memset(uart2_rx_buffer, 0, UART2_BUFFER_SIZE);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  OLED_Init();
  
  // 初始化BEEP和LED为关闭状态（高电平）
  HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
  
  // 初始化串口2
  MX_USART2_UART_Init();
  
  // 从FLASH读取阈值
  uint16_t saved_threshold = FLASH_ReadThreshold();
  // 检查读取的值是否有效（0-100之间）
  if(saved_threshold <= 100)
  {
    threshold = saved_threshold;
  }
  else
  {
    // 如果无效，使用默认值并保存
    threshold = 5; // 降低默认阈值以便测试
    FLASH_WriteThreshold(threshold);
  }
  
  // 显示初始人流计数、阈值和蓝牙连接状态
  OLED_Clear();
  OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
  OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
  OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
  OLED_ShowString(0, 8, (uint8_t*)"Threshold:", 8, 1);
  OLED_ShowNum(64, 8, threshold, 3, 8, 1);
  OLED_ShowString(0, 16, (uint8_t*)"BLE: Disconnected", 8, 1);
  OLED_Refresh();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // 检测HC_SR505状态
    if(HAL_GPIO_ReadPin(HC_SR505_GPIO_Port, HC_SR505_Pin) == GPIO_PIN_SET)
    {
      // 高电平，计数加1
      if(hc_sr505_count < HC_SR505_THRESHOLD)
      {
        hc_sr505_count++;
      }
      
      // 达到阈值，设置有效标志
      if(hc_sr505_count >= HC_SR505_THRESHOLD)
      {
        hc_sr505_valid = 1;
      }
    }
    else
    {
      // 低电平，重置计数和标志
      hc_sr505_count = 0;
      hc_sr505_valid = 0;
    }
    

    
    // 检查是否超过阈值，控制蜂鸣器和LED
    if(pedestrian_count > threshold && !alarm_active && !alarm_triggered)
    {
      // 激活报警
      alarm_active = 1;
      alarm_start_time = HAL_GetTick();
      // 拉低BEEP和LED，使其工作
      HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
      // 设置报警已触发标志
      alarm_triggered = 1;
      // 更新显示，显示报警信息
      if(!setting_mode)
      {
        OLED_Clear();
        OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
        OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
        OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
        OLED_ShowString(0, 8, (uint8_t*)"ALARM! Over threshold", 8, 1);
        OLED_ShowString(0, 16, (uint8_t*)"Threshold:", 8, 1);
        OLED_ShowNum(64, 16, threshold, 3, 8, 1);
        OLED_ShowString(0, 24, (uint8_t*)"Alarm triggered", 8, 1);
        OLED_Refresh();
      }
      // 强制发送警告信息，不管蓝牙是否连接
      char warning_msg[50];
      sprintf(warning_msg, "ALARM: Pedestrian count %ld over threshold %d\r\n", pedestrian_count, threshold);
      HAL_UART_Transmit(&huart2, (uint8_t*)warning_msg, strlen(warning_msg), HAL_MAX_DELAY);
    }
    
    // 检查报警是否需要关闭
    if(alarm_active && (HAL_GetTick() - alarm_start_time > ALARM_DURATION))
    {
      // 关闭报警
      alarm_active = 0;
      // 拉高BEEP和LED，使其关闭
      HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
      // 更新显示
      if(!setting_mode)
      {
        OLED_Clear();
        OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
        OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
        OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
        OLED_ShowString(0, 8, (uint8_t*)"Alarm stopped", 8, 1);
        // 显示蓝牙连接状态
        if(ble_connected)
        {
          OLED_ShowString(0, 16, (uint8_t*)"BLE: Connected", 8, 1);
        }
        else
        {
          OLED_ShowString(0, 16, (uint8_t*)"BLE: Disconnected", 8, 1);
        }
        OLED_Refresh();
      }
    }
    
    // 短暂延时，避免频繁检测
    HAL_Delay(50);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
  * @brief  EXTI line detection callback.
  * @param  GPIO_Pin: Specifies the port pin connected to corresponding EXTI line.
  * @retval None
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  // 防抖处理
  uint32_t current_time = HAL_GetTick();
  if(current_time - last_interrupt_time > DEBOUNCE_TIME)
  {
    if(GPIO_Pin == GUANG_DIAN_Pin)
    {
      // 只有当HC_SR505有效且不在设置模式时才计数
      if(hc_sr505_valid && !setting_mode)
      {
        // 人流计数加1
        pedestrian_count++;
        
        // 清除OLED显示
        OLED_Clear();
        
        // 显示人流数量
        OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
        OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
        OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
        
        // 显示蓝牙连接状态
        if(ble_connected)
        {
          OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
        }
        else
        {
          OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
        }
        
        // 刷新OLED显示
        OLED_Refresh();
      }
    }
    else if(GPIO_Pin == KEY1_Pin)
    {
      // KEY1按下，切换设置模式
      if(setting_mode)
      {
        // 退出设置模式
        ExitSettingMode();
      }
      else
      {
        // 进入设置模式
        EnterSettingMode();
      }
    }
    else if(GPIO_Pin == KEY2_Pin)
    {
      if(setting_mode)
      {
        // 设置模式：减10
        if(threshold > 0)
        {
          threshold -= 10;
        }
        else
        {
          threshold = 100;
        }
        UpdateThresholdDisplay();
      }
      else
      {
        // 正常模式：清理人数，重置报警触发标志
        pedestrian_count = 0;
        alarm_triggered = 0;
        // 更新显示
        OLED_Clear();
        OLED_ShowString(0, 0, (uint8_t*)"Count:", 8, 1);
        OLED_ShowNum(40, 0, pedestrian_count, 4, 8, 1);
        OLED_ShowString(72, 0, (uint8_t*)"people", 8, 1);
        // 显示蓝牙连接状态
        if(ble_connected)
        {
          OLED_ShowString(0, 8, (uint8_t*)"BLE: Connected", 8, 1);
        }
        else
        {
          OLED_ShowString(0, 8, (uint8_t*)"BLE: Disconnected", 8, 1);
        }
        OLED_Refresh();
      }
    }
    else if(GPIO_Pin == KEY3_Pin && setting_mode)
    {
      // KEY3按下，加10
      if(threshold < 100)
      {
        threshold += 10;
      }
      else
      {
        threshold = 0;
      }
      UpdateThresholdDisplay();
    }
    
    // 更新上次中断时间
    last_interrupt_time = current_time;
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
