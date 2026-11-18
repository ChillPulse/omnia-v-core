#include "stm32f4xx_hal.h"

DMA_HandleTypeDef hdma_i2s3_ext_rx;
DMA_HandleTypeDef hdma_spi3_tx;

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hi2s->Instance == SPI3)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_SPI3_CLK_ENABLE();
        __HAL_RCC_DMA1_CLK_ENABLE();

        /* PA15 = I2S3_WS */
        GPIO_InitStruct.Pin = GPIO_PIN_15;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF6_SPI3;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* PB3 = I2S3_CK, PB5 = I2S3_SD */
        GPIO_InitStruct.Pin = GPIO_PIN_3 | GPIO_PIN_5;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF6_SPI3;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* PB4 = I2S3ext_SD */
        GPIO_InitStruct.Pin = GPIO_PIN_4;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = (uint8_t)0x07;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* SPI3_TX -> DMA1 Stream5 Channel0 */
        hdma_spi3_tx.Instance = DMA1_Stream5;
        hdma_spi3_tx.Init.Channel = DMA_CHANNEL_0;
        hdma_spi3_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
        hdma_spi3_tx.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_spi3_tx.Init.MemInc = DMA_MINC_ENABLE;
        hdma_spi3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        hdma_spi3_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
        hdma_spi3_tx.Init.Mode = DMA_CIRCULAR;
        hdma_spi3_tx.Init.Priority = DMA_PRIORITY_HIGH;
        hdma_spi3_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&hdma_spi3_tx) != HAL_OK)
        {
            while (1) {}
        }

        __HAL_LINKDMA(hi2s, hdmatx, hdma_spi3_tx);

        /* I2S3_EXT_RX -> DMA1 Stream2 Channel2 */
        hdma_i2s3_ext_rx.Instance = DMA1_Stream2;
        hdma_i2s3_ext_rx.Init.Channel = DMA_CHANNEL_2;
        hdma_i2s3_ext_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
        hdma_i2s3_ext_rx.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_i2s3_ext_rx.Init.MemInc = DMA_MINC_ENABLE;
        hdma_i2s3_ext_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        hdma_i2s3_ext_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
        hdma_i2s3_ext_rx.Init.Mode = DMA_CIRCULAR;
        hdma_i2s3_ext_rx.Init.Priority = DMA_PRIORITY_HIGH;
        hdma_i2s3_ext_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&hdma_i2s3_ext_rx) != HAL_OK)
        {
            while (1) {}
        }

        __HAL_LINKDMA(hi2s, hdmarx, hdma_i2s3_ext_rx);

        HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 1, 0);
        HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);

        HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 1, 1);
        HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    }
}

void HAL_I2S_MspDeInit(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI3)
    {
        __HAL_RCC_SPI3_CLK_DISABLE();

        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_15);
        HAL_GPIO_DeInit(GPIOB, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5);

        HAL_DMA_DeInit(hi2s->hdmatx);
        HAL_DMA_DeInit(hi2s->hdmarx);

        HAL_NVIC_DisableIRQ(DMA1_Stream2_IRQn);
        HAL_NVIC_DisableIRQ(DMA1_Stream5_IRQn);
    }
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance == USART2)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_USART2_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
    }
}
