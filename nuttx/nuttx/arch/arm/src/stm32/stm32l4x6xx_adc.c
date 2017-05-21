/****************************************************************************
 * arch/arm/src/stm32/stm32l4x6xx_adc.c
 *
 *   Copyright (C) 2015 Motorola Mobility, LLC. All rights reserved.
 *   Copyright (C) 2011, 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           Diego Sanchez <dsanchez@nx-engineering.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/analog/adc.h>
#include <nuttx/power/pm.h>

#include "up_internal.h"
#include "up_arch.h"

#include "chip.h"
#include "stm32.h"
#include "stm32_adc.h"

/* ADC "upper half" support must be enabled */

#ifdef CONFIG_ADC

/* Some ADC peripheral must be enabled */

#if defined(CONFIG_STM32_ADC1) || defined(CONFIG_STM32_ADC2) || defined(CONFIG_STM32_ADC3)

/* This implementation is for the STM32 L4 only */

#if defined(CONFIG_STM32_STM32L4X6)

#ifdef ADC_HAVE_TIMER
#  warning "ADC timers not yet supported"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The maximum number of channels that can be sampled.  If dma support is
 * not enabled, then only a single channel can be sampled.  Otherwise,
 * data overruns would occur.
 */

#ifdef CONFIG_ADC_DMA
#  define ADC_MAX_SAMPLES 16
#else
#  define ADC_MAX_SAMPLES 16
#endif

#define container_of(x, s, f) ((void*) ((char*)(x) - offsetof(s, f)))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of one ADC block */

struct stm32_dev_s
{
  uint8_t  irq;       /* Interrupt generated by this ADC block */
  uint8_t  nchannels; /* Number of channels */
  uint8_t  intf;      /* ADC interface number */
  uint8_t  current;   /* Current ADC channel being converted */
  xcpt_t   isr;       /* Interrupt handler for this ADC block */
  uint32_t base;      /* Base address of registers unique to this ADC block */
  uint8_t  chanlist[ADC_MAX_SAMPLES];
#ifdef CONFIG_PM
  struct pm_callback_s pm_callback;
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* ADC Register access */

static uint32_t adc_getreg(struct stm32_dev_s *priv, int offset);
static void adc_putreg(struct stm32_dev_s *priv, int offset, uint32_t value);
static void adc_rccreset(struct stm32_dev_s *priv, bool reset);

/* ADC Interrupt Handler */

static int adc_interrupt(FAR struct adc_dev_s *dev);
#if defined(CONFIG_STM32_ADC1) || defined(CONFIG_STM32_ADC2)
static int adc12_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_STM32_ADC3
static int adc3_interrupt(int irq, void *context);
#endif

/* ADC Driver Methods */

static void adc_reset(FAR struct adc_dev_s *dev);
static int  adc_setup(FAR struct adc_dev_s *dev);
static void adc_shutdown(FAR struct adc_dev_s *dev);
static void adc_rxint(FAR struct adc_dev_s *dev, bool enable);
static int  adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg);
static void adc_enable(FAR struct stm32_dev_s *priv);
static void adc_startconv(FAR struct stm32_dev_s *priv, bool enable);

#ifdef CONFIG_PM
static int  pm_prepare(struct pm_callback_s *cb, enum pm_state_e state);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ADC interface operations */

static const struct adc_ops_s g_adcops =
{
  .ao_reset    = adc_reset,
  .ao_setup    = adc_setup,
  .ao_shutdown = adc_shutdown,
  .ao_rxint    = adc_rxint,
  .ao_ioctl    = adc_ioctl,
};

/* ADC1 state */

#ifdef CONFIG_STM32_ADC1
static struct stm32_dev_s g_adcpriv1 =
{
  .irq         = STM32_IRQ_ADC12,
  .isr         = adc12_interrupt,
  .intf        = 1,
  .base        = STM32_ADC1_BASE,
#ifdef CONFIG_PM
  .pm_callback =
    {
      .prepare = pm_prepare,
    }
#endif
};

static struct adc_dev_s g_adcdev1 =
{
  .ad_ops      = &g_adcops,
  .ad_priv     = &g_adcpriv1,
};
#endif

/* ADC2 state */

#ifdef CONFIG_STM32_ADC2
static struct stm32_dev_s g_adcpriv2 =
{
  .irq         = STM32_IRQ_ADC12,
  .isr         = adc12_interrupt,
  .intf        = 2,
  .base        = STM32_ADC2_BASE,
#ifdef CONFIG_PM
  .pm_callback =
    {
      .prepare = pm_prepare,
    }
#endif
};

static struct adc_dev_s g_adcdev2 =
{
  .ad_ops      = &g_adcops,
  .ad_priv     = &g_adcpriv2,
};
#endif

/* ADC3 state */

#ifdef CONFIG_STM32_ADC3
static struct stm32_dev_s g_adcpriv3 =
{
  .irq         = STM32_IRQ_ADC3,
  .isr         = adc3_interrupt,
  .intf        = 3,
  .base        = STM32_ADC3_BASE,
#ifdef CONFIG_PM
  .pm_callback =
    {
      .prepare = pm_prepare,
    }
#endif
};

static struct adc_dev_s g_adcdev3 =
{
  .ad_ops      = &g_adcops,
  .ad_priv     = &g_adcpriv3,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: adc_getreg
 *
 * Description:
 *   Read the value of an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *   Register value
 *
 ****************************************************************************/

static uint32_t adc_getreg(struct stm32_dev_s *priv, int offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: adc_putreg
 *
 * Description:
 *   Write the value to an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to write
 *   value - Value to write to register
 *
 ****************************************************************************/

static void adc_putreg(struct stm32_dev_s *priv, int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

#if CONFIG_PM
/****************************************************************************
 * Name: pm_prepare
 *
 * Description:
 *   Called by power management framework when it wants to enter low power
 *   states. Check if ADC is in progress and if so prevent from entering STOP.
 *
 ****************************************************************************/

static int pm_prepare(struct pm_callback_s *cb, enum pm_state_e state)
{
  FAR struct stm32_dev_s *priv =
      container_of(cb, struct stm32_dev_s, pm_callback);
  uint32_t regval;

  regval = adc_getreg(priv, STM32_ADC_CR_OFFSET);
  if ((state >= PM_IDLE) && (regval & ADC_CR_ADSTART))
      return -EBUSY;

  return OK;
}
#endif

/****************************************************************************
 * Name: adc_wdog_enable
 *
 * Description:
 *   Enable the analog watchdog.
 *
 ****************************************************************************/

static void adc_wdog_enable(struct stm32_dev_s *priv)
{
  uint32_t regval;

  /* Initialize the Analog watchdog enable */

  regval  = adc_getreg(priv, STM32_ADC_CFGR_OFFSET);
  regval |= ADC_CFGR_AWD1EN | ADC_CFGR_CONT | ADC_CFGR_OVRMOD;
  adc_putreg(priv, STM32_ADC_CFGR_OFFSET, regval);

  /* Switch to analog watchdog interrupt */

  regval = adc_getreg(priv, STM32_ADC_IER_OFFSET);
  regval |= ADC_INT_AWD1;
  regval &= ~ADC_INT_EOC;
  adc_putreg(priv, STM32_ADC_IER_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_startconv
 *
 * Description:
 *   Start (or stop) the ADC conversion process
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   enable - True: Start conversion
 *
 ****************************************************************************/

static void adc_startconv(struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval;

  avdbg("enable: %d\n", enable);

  regval = adc_getreg(priv, STM32_ADC_CR_OFFSET);
  if (enable)
    {
      /* Start conversion of regular channels */

      regval |= ADC_CR_ADSTART;
    }
  else
    {
      /* Disable the conversion of regular channels */

      regval |= ADC_CR_ADSTP;
    }
  adc_putreg(priv, STM32_ADC_CR_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_rccreset
 *
 * Description:
 *   Deinitializes the ADCx peripheral registers to their default
 *   reset values.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   reset - Condition, set or reset
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_rccreset(struct stm32_dev_s *priv, bool reset)
{
  irqstate_t flags;
  uint32_t regval;

  /* Disable interrupts.  This is necessary because the APB2RTSR register
   * is used by several different drivers.
   */

  flags = irqsave();

  /* Set or clear the selected bit in the APB2 reset register */

  regval = getreg32(STM32_RCC_AHB2RSTR);
  if (reset)
    {
      /* Enable  ADC reset state */

      regval |= RCC_AHB2RSTR_ADCRST;
    }
  else
    {
      /* Release ADC from reset state */

      regval &= ~RCC_AHB2RSTR_ADCRST;
    }
  putreg32(regval, STM32_RCC_AHB2RSTR);
  irqrestore(flags);
}

/*******************************************************************************
 * Name: adc_enable
 *
 * Description    : Enables or disables the specified ADC peripheral.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *
 *******************************************************************************/

static void adc_enable(FAR struct stm32_dev_s *priv)
{
  uint32_t regval;

  avdbg("enter\n");

  regval = adc_getreg(priv, STM32_ADC_CR_OFFSET);

  /* Exit deep power down mode and enable voltage regulator */

  regval &= ~ADC_CR_DEEPPWD;
  regval |= ADC_CR_ADVREGEN;
  adc_putreg(priv, STM32_ADC_CR_OFFSET, regval);

  /* Wait for voltage regulator to power up */

  up_udelay(20);

  /* Enable ADC calibration */

  regval |= ADC_CR_ADCAL;
  adc_putreg(priv, STM32_ADC_CR_OFFSET, regval);

  /* Wait for calibration to complete */

  while (adc_getreg(priv, STM32_ADC_CR_OFFSET) & ADC_CR_ADCAL);

  /* Enable ADC */

  regval  = adc_getreg(priv, STM32_ADC_CR_OFFSET);
  regval |= ADC_CR_ADEN;
  adc_putreg(priv, STM32_ADC_CR_OFFSET, regval);

  /* Wait for hardware to be ready for conversions */

  while (!(adc_getreg(priv, STM32_ADC_ISR_OFFSET) & ADC_INT_ADRDY));
}

/****************************************************************************
 * Name: adc_reset
 *
 * Description:
 *   Reset the ADC device.  Called early to initialize the hardware. This
 *   is called, before adc_setup() and on error conditions.
 *
 ****************************************************************************/

static void adc_reset(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;

  avdbg("intf: ADC%d\n", priv->intf);

  /* Enable ADC reset state */

  adc_rccreset(priv, true);

  /* Release ADC from reset state */

  adc_rccreset(priv, false);
}

/****************************************************************************
 * Name: adc_setup
 *
 * Description:
 *   Configure the ADC. This method is called the first time that the ADC
 *   device is opened.  This will occur when the port is first opened.
 *   This setup includes configuring and attaching ADC interrupts.  Interrupts
 *   are all disabled upon return.
 *
 ****************************************************************************/

static int adc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  int ret;
  irqstate_t flags;
  uint32_t regval;
  int offset;
  int i;

  /* Attach the ADC interrupt */

  ret = irq_attach(priv->irq, priv->isr);
  if (ret != OK)
      return ret;

  flags = irqsave();

  /* Make sure that the ADC device is in the powered up, reset state */

  adc_reset(dev);

  /* Initialize the same sample time (640.5 cycles) for each ADC */

  adc_putreg(priv, STM32_ADC_SMPR1_OFFSET, 0x3fffffff);
  adc_putreg(priv, STM32_ADC_SMPR2_OFFSET, 0x07ffffff);

  /* Configuration of the channel conversions */

  DEBUGASSERT(priv->nchannels <= ADC_MAX_SAMPLES);

#ifdef __GNUC__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
#endif
  regval = adc_getreg(priv, STM32_ADC_SQR4_OFFSET);
  for (i = 14, offset = 0; i < priv->nchannels && i < 16; i++, offset += 6)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }
  adc_putreg(priv, STM32_ADC_SQR4_OFFSET, regval);

  regval = adc_getreg(priv, STM32_ADC_SQR3_OFFSET);
  for (i = 9, offset = 0; i < priv->nchannels && i < 14; i++, offset += 6)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }
  adc_putreg(priv, STM32_ADC_SQR3_OFFSET, regval);

  regval = adc_getreg(priv, STM32_ADC_SQR2_OFFSET);
  for (i = 4, offset = 0; i < priv->nchannels && i < 9; i++, offset += 6)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }
  adc_putreg(priv, STM32_ADC_SQR2_OFFSET, regval);

  regval = adc_getreg(priv, STM32_ADC_SQR1_OFFSET);
  for (i = 0, offset = 6; i < priv->nchannels && i < 4; i++, offset += 6)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }

  /* Set the number of conversions */

  regval |= (((uint32_t)priv->nchannels-1) << ADC_SQR1_L_SHIFT);
  adc_putreg(priv, STM32_ADC_SQR1_OFFSET, regval);
#ifdef __GNUC__
# pragma GCC diagnostic pop
#endif

  /* Set the channel index of the first conversion */

  priv->current = 0;

  /* Set ADEN to wake up the ADC from Power Down state. */

  adc_enable(priv);

  irqrestore(flags);

  avdbg("ISR:   0x%08x CR:    0x%08x CFGR:  0x%08x CFGR2: 0x%08x\n",
        adc_getreg(priv, STM32_ADC_ISR_OFFSET),
        adc_getreg(priv, STM32_ADC_CR_OFFSET),
        adc_getreg(priv, STM32_ADC_CFGR_OFFSET),
        adc_getreg(priv, STM32_ADC_CFGR2_OFFSET));
  avdbg("SQR1:  0x%08x SQR2:  0x%08x SQR3:  0x%08x SQR4:  0x%08x\n",
        adc_getreg(priv, STM32_ADC_SQR1_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR2_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR3_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR4_OFFSET));
  avdbg("CCR:   0x%08x\n",
        getreg32(STM32_ADC_CCR));

  /* Enable the ADC interrupt */

  avdbg("Enable the ADC interrupt: irq=%d\n", priv->irq);
  up_enable_irq(priv->irq);

  return OK;
}

/****************************************************************************
 * Name: adc_shutdown
 *
 * Description:
 *   Disable the ADC.  This method is called when the ADC device is closed.
 *   This method reverses the operation the setup method.
 *
 ****************************************************************************/

static void adc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;

  /* Disable ADC interrupts and detach the ADC interrupt handler */

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  /* Disable and reset the ADC module */

  adc_reset(dev);
}

/****************************************************************************
 * Name: adc_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts.
 *
 * Input Parameters:
 *   enable - True to enable interrupts, false to disable
 *
 ****************************************************************************/

static void adc_rxint(FAR struct adc_dev_s *dev, bool enable)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t regval;

  avdbg("intf: %d enable: %d\n", priv->intf, enable);

  regval = adc_getreg(priv, STM32_ADC_IER_OFFSET);
  if (enable)
    {
      /* Enable end of conversion interrupt */

      regval |= ADC_INT_EOC;
    }
  else
    {
      /* Disable all interrupts */

      regval &= ~ADC_INT_MASK;
    }
  adc_putreg(priv, STM32_ADC_IER_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method.
 *
 ****************************************************************************/

static int adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg)
{
#ifdef CONFIG_STM32_ADC_SWTRIG
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t regval;
  uint32_t tmp;
#endif
  int ret = OK;

  avdbg("cmd=0x%04x arg=%ld\n", cmd, arg);

  switch (cmd)
    {
#ifdef CONFIG_STM32_ADC_SWTRIG
      case ANIOC_TRIGGER: /* Software trigger */
        {
          /* Start conversion */

          adc_startconv(priv, true);
        }
        break;

      case ANIOC_WDOG_UPPER: /* Set watchdog upper threshold */
        {
          regval = adc_getreg(priv, STM32_ADC_TR1_OFFSET);

          /* Verify new upper threshold greater than lower threshold */

          tmp = (regval & ADC_TR1_LT_MASK) >> ADC_TR1_LT_SHIFT;
          if (arg < tmp)
            {
              ret = -EINVAL;
              break;
            }

          /* Set the watchdog threshold register */

          regval &= ~ADC_TR1_HT_MASK;
          regval |= ((arg << ADC_TR1_HT_SHIFT) & ADC_TR1_HT_MASK);
          adc_putreg(priv, STM32_ADC_TR1_OFFSET, regval);

          /* Ensure analog watchdog is enabled */

          adc_wdog_enable(priv);
        }
        break;

      case ANIOC_WDOG_LOWER: /* Set watchdog lower threshold */
        {
          regval = adc_getreg(priv, STM32_ADC_TR1_OFFSET);

          /* Verify new lower threshold less than upper threshold */

          tmp = (regval & ADC_TR1_HT_MASK) >> ADC_TR1_HT_SHIFT;
          if (arg > tmp)
            {
              ret = -EINVAL;
              break;
            }

          /* Set the watchdog threshold register */

          regval &= ~ADC_TR1_LT_MASK;
          regval |= ((arg << ADC_TR1_LT_SHIFT) & ADC_TR1_LT_MASK);
          adc_putreg(priv, STM32_ADC_TR1_OFFSET, regval);

          /* Ensure analog watchdog is enabled */

          adc_wdog_enable(priv);
        }
      break;
#endif

      /* Unsupported or invalid command */

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: adc_interrupt
 *
 * Description:
 *   Common ADC interrupt handler.
 *
 ****************************************************************************/

static int adc_interrupt(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t adcisr;
  int32_t  value;

  adcisr = adc_getreg(priv, STM32_ADC_ISR_OFFSET);

  /* AWD: Analog Watchdog */

  if ((adcisr & ADC_INT_AWD1) != 0)
    {
      value  = adc_getreg(priv, STM32_ADC_DR_OFFSET);
      value &= ADC_DR_MASK;

      alldbg("Analog Watchdog, Value (0x%03x) out of range!\n", value);

      /* Stop ADC conversions to avoid continuous interrupts */

      adc_startconv(priv, false);
    }

  /* EOC: End of conversion */

  if ((adcisr & ADC_INT_EOC) != 0)
    {
      /* Read the converted value and clear EOC bit
       * (It is cleared by reading the ADC_DR)
       */

      value  = adc_getreg(priv, STM32_ADC_DR_OFFSET);
      value &= ADC_DR_MASK;

      /* Give the ADC data to the ADC driver.  adc_receive accepts 3 parameters:
       *
       * 1) The first is the ADC device instance for this ADC block.
       * 2) The second is the channel number for the data, and
       * 3) The third is the converted data for the channel.
       */

      adc_receive(dev, priv->chanlist[priv->current], value);

      /* Set the channel number of the next channel that will complete conversion */

      priv->current++;

      if (priv->current >= priv->nchannels)
        {
          /* Restart the conversion sequence from the beginning */

          priv->current = 0;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: adc12_interrupt
 *
 * Description:
 *   ADC12 interrupt handler for the STM32 L4 family.
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_ADC1) || defined(CONFIG_STM32_ADC2)
static int adc12_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* Check for pending ADC1 interrupts */

#ifdef CONFIG_STM32_ADC1
  regval  = getreg32(STM32_ADC1_ISR);
  pending = regval & ADC_INT_MASK;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev1);

      /* Clear interrupts */
      putreg32(regval, STM32_ADC1_ISR);
    }
#endif

  /* Check for pending ADC2 interrupts */

#ifdef CONFIG_STM32_ADC2
  regval  = getreg32(STM32_ADC2_ISR);
  pending = regval & ADC_INT_MASK;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev2);

      /* Clear interrupts */
      putreg32(regval, STM32_ADC2_ISR);
    }
#endif
  return OK;
}
#endif

/****************************************************************************
 * Name: adc3_interrupt
 *
 * Description:
 *   ADC3 interrupt handler for the STM32 L4 family.
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_ADC3
static int adc3_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* Check for pending ADC3 interrupts */

  regval  = getreg32(STM32_ADC3_ISR);
  pending = regval & ADC_INT_MASK;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev3);

      /* Clear interrupts */
      putreg32(regval, STM32_ADC3_ISR);
    }

  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_adcinitialize
 *
 * Description:
 *   Initialize the ADC.
 *
 *   The logic is, save nchannels : # of channels (conversions) in ADC_SQR1_L
 *   Then, take the chanlist array and store it in the SQR Regs,
 *     chanlist[0] -> ADC_SQR3_SQ1
 *     chanlist[1] -> ADC_SQR3_SQ2
 *     ...
 *     chanlist[15]-> ADC_SQR1_SQ16
 *
 *   up to
 *     chanlist[nchannels]
 *
 * Input Parameters:
 *   intf      - Could be {1,2,3} for ADC1, ADC2, or ADC3
 *   chanlist  - The list of channels
 *   nchannels - Number of channels
 *
 * Returned Value:
 *   Valid ADC device structure reference on succcess; a NULL on failure
 *
 ****************************************************************************/

struct adc_dev_s *stm32_adcinitialize(int intf, const uint8_t *chanlist,
                                      int nchannels)
{
  FAR struct adc_dev_s   *dev;
  FAR struct stm32_dev_s *priv;

  avdbg("intf: %d nchannels: %d\n", intf, nchannels);

#ifdef CONFIG_STM32_ADC1
  if (intf == 1)
    {
      avdbg("ADC1 Selected\n");
      dev = &g_adcdev1;
    }
  else
#endif
#ifdef CONFIG_STM32_ADC2
  if (intf == 2)
    {
      avdbg("ADC2 Selected\n");
      dev = &g_adcdev2;
    }
  else
#endif
#ifdef CONFIG_STM32_ADC3
  if (intf == 3)
    {
      avdbg("ADC3 Selected\n");
      dev = &g_adcdev3;
    }
  else
#endif
    {
      adbg("No ADC interface defined\n");
      return NULL;
    }

  /* Configure the selected ADC */

  priv = dev->ad_priv;

  DEBUGASSERT(nchannels <= ADC_MAX_SAMPLES);
  priv->nchannels = nchannels;

  memcpy(priv->chanlist, chanlist, nchannels);

#ifdef CONFIG_PM
  if (pm_register(&priv->pm_callback) != OK)
    {
      adbg("Power management registration failed\n");
      return NULL;
    }
#endif

  return dev;
}

#endif /* CONFIG_STM32_STM32L4X6 */
#endif /* CONFIG_STM32_ADC || CONFIG_STM32_ADC2 || CONFIG_STM32_ADC3 */
#endif /* CONFIG_ADC */
