/*
* sunxi_can.c - CAN bus controller driver for sun7i and sun4i
*
* Copyright (c) 2013 Peter Chen
*  - driver for sun7i
* Copyright (c) 2017 Oleg Strelkov <o.strelkov@gmail.com>
*  - modifications for sun4i
*
* Copyright (c) 2013 Inmotion Co,. LTD
* All right reserved.
*
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/clk.h>

#include <linux/can/dev.h>
#include <linux/can/error.h>

#include <plat/sys_config.h>
#include <mach/irqs.h>

#include "sunxi_can.h"

#define DRV_NAME "sunxi_can"

MODULE_AUTHOR("Peter Chen <xingkongcp@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION(DRV_NAME "CAN netdevice driver");

static struct net_device *sunxican_dev;
static struct can_bittiming_const sunxi_can_bittiming_const = {
        .name = DRV_NAME,
        .tseg1_min = 1,
        .tseg1_max = 16,
        .tseg2_min = 1,
        .tseg2_max = 8,
        .sjw_max = 4,
        .brp_min = 1,
        .brp_max = 64,
        .brp_inc = 1,
};

static void sunxi_can_write_cmdreg(struct sunxi_can_priv *priv, u8 val)
{
        unsigned long flags;

        /*
         * The command register needs some locking and time to settle
         * the write_reg() operation - especially on SMP systems.
         */
        spin_lock_irqsave(&priv->cmdreg_lock, flags);
        writel(val, CAN_CMD_ADDR);
        spin_unlock_irqrestore(&priv->cmdreg_lock, flags);
}

static int sunxi_can_is_absent(struct sunxi_can_priv *priv)
{
        return ((readl(CAN_MSEL_ADDR) & 0xFF) == 0xFF);
}

static int sunxi_can_probe(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);

        if (sunxi_can_is_absent(priv)) {
                printk(KERN_INFO "%s: probing @0x%lX failed\n",
                 DRV_NAME, dev->base_addr);
                return 0;
        }
        return -1;
}

static void set_reset_mode(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        uint32_t status = readl(CAN_MSEL_ADDR);
        int i;

        for (i = 0; i < 100; i++) {
                /* check reset bit */
                if (status & RESET_MODE) {
                        priv->can.state = CAN_STATE_STOPPED;
                        return;
                }

                writel(readl(CAN_MSEL_ADDR) | RESET_MODE, CAN_MSEL_ADDR);        /* select reset mode */
                //writel(RESET_MODE, CAN_MSEL_ADDR);        /* select reset mode */
                udelay(10);
                status = readl(CAN_MSEL_ADDR);
        }

        netdev_err(dev, "setting SUNXI_CAN into reset mode failed!\n");
}

static void set_normal_mode(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        unsigned char status = readl(CAN_MSEL_ADDR);
        int i;

        for (i = 0; i < 100; i++) {
                /* check reset bit */
                if ((status & RESET_MODE) == 0) {
                        priv->can.state = CAN_STATE_ERROR_ACTIVE;												
						
						/* enable interrupts */
                        if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING) {
								writel(0xFFFF, CAN_INTEN_ADDR);
						} else {
								writel(0xFFFF & ~BERR_IRQ_EN, CAN_INTEN_ADDR);
						}
											
						if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) {
							/* Put device into loopback mode */
							writel(readl(CAN_MSEL_ADDR) | LOOPBACK_MODE, CAN_MSEL_ADDR);
						} else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY) {
							/* Put device into listen-only mode */
							writel(readl(CAN_MSEL_ADDR) | LISTEN_ONLY_MODE, CAN_MSEL_ADDR);
						}
                        return;
                }

                /* set chip to normal mode */
                writel(readl(CAN_MSEL_ADDR) & (~RESET_MODE), CAN_MSEL_ADDR);
                udelay(10);
                status = readl(CAN_MSEL_ADDR);
        }

        netdev_err(dev, "setting SUNXI_CAN into normal mode failed!\n");
}


static void sunxi_can_start(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);

        /* leave reset mode */
        if (priv->can.state != CAN_STATE_STOPPED)
                set_reset_mode(dev);

        /* Clear error counters and error code capture */
        writel(0x0, CAN_ERRC_ADDR);

        /* leave reset mode */
        set_normal_mode(dev);
}

static int sunxi_can_set_mode(struct net_device *dev, enum can_mode mode)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);

        if (!priv->open_time)
                return -EINVAL;

        switch (mode) {
        case CAN_MODE_START:
                sunxi_can_start(dev);
                if (netif_queue_stopped(dev))
                        netif_wake_queue(dev);
                break;

        default:
                return -EOPNOTSUPP;
        }

        return 0;
}

static int sunxi_can_set_bittiming(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        struct can_bittiming *bt = &priv->can.bittiming;
        u32 cfg;

        cfg = ((bt->brp - 1) & 0x3FF)
                | (((bt->sjw - 1) & 0x3) << 14)
                | (((bt->prop_seg + bt->phase_seg1 - 1) & 0xf) << 16)
                | (((bt->phase_seg2 - 1) & 0x7) << 20);
        if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
                cfg |= 0x800000;

        netdev_info(dev, "setting BITTIMING=0x%08x\n", cfg);

        set_reset_mode(dev);                //CAN_BTIME_ADDR only writable in reset mode
        writel(cfg, CAN_BTIME_ADDR);
        set_normal_mode(dev);

        return 0;
}

static int sunxi_can_get_berr_counter(const struct net_device *dev,
                                 struct can_berr_counter *bec)
{
        bec->txerr = readl(CAN_ERRC_ADDR) & 0x000F;
        bec->rxerr = (readl(CAN_ERRC_ADDR) & 0x0F00) >> 16;

        return 0;
}

/*
* initialize sunxi_can:
* - reset chip
* - set output mode
* - set baudrate
* - enable interrupts
* - start operating mode
*/
static void chipset_init(struct net_device *dev)
{
        u32 temp_irqen;
		
        /* config pins
         * PH20-TX, PH21-RX :4 */

		if (gpio_request_ex("can_para", "can_tx") == 0 || gpio_request_ex("can_para", "can_rx") == 0 ) {
			pr_info("can request gpio fail!\n");
        }

        //enable clock
        writel(readl(0xF1C20000 + 0x6C) | (1 << 4), 0xF1C20000 + 0x6C);

        //set can controller in reset mode
        set_reset_mode(dev);

        //enable interrupt
        temp_irqen = BERR_IRQ_EN | ERR_PASSIVE_IRQ_EN
                        | OR_IRQ_EN | RX_IRQ_EN;
        writel(readl(CAN_INTEN_ADDR) | temp_irqen, CAN_INTEN_ADDR);

        //return to transfer mode
        set_normal_mode(dev);
}

/*
* transmit a CAN message
* message layout in the sk_buff should be like this:
* xx xx xx xx         ff         ll 00 11 22 33 44 55 66 77
* [ can_id ] [flags] [len] [can data (up to 8 bytes]
*/
static netdev_tx_t sunxi_can_start_xmit(struct sk_buff *skb,
                                         struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        struct can_frame *cf = (struct can_frame *)skb->data;
        uint8_t dlc;
        canid_t id;
        uint32_t temp = 0;
        uint8_t i;
        
        //wait buff ready
        while (!(readl(CAN_STA_ADDR) & TBUF_RDY));

        set_reset_mode(dev);

        writel(0xffffffff, CAN_ACPM_ADDR);

        //enter transfer mode
        set_normal_mode(dev);

        if (can_dropped_invalid_skb(dev, skb))
                return NETDEV_TX_OK;

        netif_stop_queue(dev);

        dlc = cf->can_dlc;
        id = cf->can_id;

        temp = ((id >> 30) << 6) | dlc;
        writel(temp, CAN_BUF0_ADDR);
        if (id & CAN_EFF_FLAG) {/* extern frame */
                writel(0xFF & (id >> 21), CAN_BUF1_ADDR);        //id28~21
                writel(0xFF & (id >> 13), CAN_BUF2_ADDR);         //id20~13
                writel(0xFF & (id >> 5), CAN_BUF3_ADDR);         //id12~5
                writel((id & 0x1F) << 3, CAN_BUF4_ADDR);         //id4~0
                
                for (i = 0; i < dlc; i++) {
                        writel(cf->data[i], CAN_BUF5_ADDR + i * 4);
                }
        } else {                /* standard frame*/        
                writel(0xFF & (id >> 3), CAN_BUF1_ADDR);         //id28~21
                writel((id & 0x7) << 5, CAN_BUF2_ADDR);                //id20~13
                
                for (i = 0; i < dlc; i++) {
                        writel(cf->data[i], CAN_BUF3_ADDR + i * 4);
                }
        }

        can_put_echo_skb(skb, dev, 0);

        while (!(readl(CAN_STA_ADDR) & TBUF_RDY));
        sunxi_can_write_cmdreg(priv, TRANS_REQ);

        return NETDEV_TX_OK;
}

static void sunxi_can_rx(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        struct net_device_stats *stats = &dev->stats;
        struct can_frame *cf;
        struct sk_buff *skb;
        uint8_t fi;
        canid_t id;
        int i;

        /* create zero'ed CAN frame buffer */
        skb = alloc_can_skb(dev, &cf);
        if (skb == NULL)
                return;

        fi = readl(CAN_BUF0_ADDR);
        cf->can_dlc = get_can_dlc(fi & 0x0F);
        if (fi >> 7) {
                /* extended frame format (EFF) */
                id = (readl(CAN_BUF1_ADDR) << 21)        //id28~21
                 | (readl(CAN_BUF2_ADDR) << 13)        //id20~13
                 | (readl(CAN_BUF3_ADDR) << 5)        //id12~5
                 | ((readl(CAN_BUF4_ADDR) >> 3) & 0x1f);        //id4~0
                id |= CAN_EFF_FLAG;

                if ((fi >> 6) & 0x1) {        /* remote transmission request */
                id |= CAN_RTR_FLAG;
                } else {
                        for (i = 0; i < cf->can_dlc; i++)
                                cf->data[i] = readl(CAN_BUF5_ADDR + i * 4);
                }
        } else {
                /* standard frame format (SFF) */
                id = (readl(CAN_BUF1_ADDR) << 3)        //id28~21
                 | ((readl(CAN_BUF2_ADDR) >> 5) & 0x7);        //id20~18

                if ((fi >> 6) & 0x1) {        /* remote transmission request */
                id |= CAN_RTR_FLAG;
                } else {
                        for (i = 0; i < cf->can_dlc; i++)
                                cf->data[i] = readl(CAN_BUF3_ADDR + i * 4);
                }
        }

        cf->can_id = id;

        /* release receive buffer */
        sunxi_can_write_cmdreg(priv, RELEASE_RBUF);

        netif_rx(skb);

        stats->rx_packets++;
        stats->rx_bytes += cf->can_dlc;
}

static int sunxi_can_err(struct net_device *dev, uint8_t isrc, uint8_t status)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        struct net_device_stats *stats = &dev->stats;
        struct can_frame *cf;
        struct sk_buff *skb;
        enum can_state state = priv->can.state;
        uint32_t ecc, alc;

        skb = alloc_can_err_skb(dev, &cf);
        if (skb == NULL)
                return -ENOMEM;

        if (isrc & DATA_ORUNI) {
                /* data overrun interrupt */
                netdev_dbg(dev, "data overrun interrupt\n");
                cf->can_id |= CAN_ERR_CRTL;
                cf->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;
                stats->rx_over_errors++;
                stats->rx_errors++;
                sunxi_can_write_cmdreg(priv, CLEAR_DOVERRUN);        /* clear bit */
        }

        if (isrc & ERR_WRN) {
                /* error warning interrupt */
                netdev_dbg(dev, "error warning interrupt\n");

                if (status & BUS_OFF) {
                        state = CAN_STATE_BUS_OFF;
                        cf->can_id |= CAN_ERR_BUSOFF;
                        can_bus_off(dev);
                } else if (status & ERR_STA) {
                        state = CAN_STATE_ERROR_WARNING;
                } else
                        state = CAN_STATE_ERROR_ACTIVE;
        }
        if (isrc & BUS_ERR) {
                /* bus error interrupt */
                priv->can.can_stats.bus_error++;
                stats->rx_errors++;

                ecc = readl(CAN_STA_ADDR);

                cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

                if(ecc & BIT_ERR)
                        cf->data[2] |= CAN_ERR_PROT_BIT;
                else if (ecc & FORM_ERR)
                        cf->data[2] |= CAN_ERR_PROT_FORM;
                else if (ecc & STUFF_ERR)
                        cf->data[2] |= CAN_ERR_PROT_STUFF;
                else {
                        cf->data[2] |= CAN_ERR_PROT_UNSPEC;
                        cf->data[3] = (ecc & ERR_SEG_CODE) >> 16;
                }
                /* Error occurred during transmission? */
                if ((ecc & ERR_DIR) == 0)
                        cf->data[2] |= CAN_ERR_PROT_TX;
        }
        if (isrc & ERR_PASSIVE) {
                /* error passive interrupt */
                netdev_dbg(dev, "error passive interrupt\n");
                if (status & ERR_STA)
                        state = CAN_STATE_ERROR_PASSIVE;
                else
                        state = CAN_STATE_ERROR_ACTIVE;
        }
        if (isrc & ARB_LOST) {
                /* arbitration lost interrupt */
                netdev_dbg(dev, "arbitration lost interrupt\n");
                alc = readl(CAN_STA_ADDR);
                priv->can.can_stats.arbitration_lost++;
                stats->tx_errors++;
                cf->can_id |= CAN_ERR_LOSTARB;
                cf->data[0] = (alc & 0x1f) >> 8;
        }

        if (state != priv->can.state && (state == CAN_STATE_ERROR_WARNING ||
                                         state == CAN_STATE_ERROR_PASSIVE)) {
                uint8_t rxerr = (readl(CAN_ERRC_ADDR) >> 16) & 0xFF;
                uint8_t txerr = readl(CAN_ERRC_ADDR) & 0xFF;
                cf->can_id |= CAN_ERR_CRTL;
                if (state == CAN_STATE_ERROR_WARNING) {
                        priv->can.can_stats.error_warning++;
                        cf->data[1] = (txerr > rxerr) ?
                                CAN_ERR_CRTL_TX_WARNING :
                                CAN_ERR_CRTL_RX_WARNING;
                } else {
                        priv->can.can_stats.error_passive++;
                        cf->data[1] = (txerr > rxerr) ?
                                CAN_ERR_CRTL_TX_PASSIVE :
                                CAN_ERR_CRTL_RX_PASSIVE;
                }
                cf->data[6] = txerr;
                cf->data[7] = rxerr;
        }

        priv->can.state = state;

        netif_rx(skb);

        stats->rx_packets++;
        stats->rx_bytes += cf->can_dlc;

        return 0;
}

irqreturn_t sunxi_can_interrupt(int irq, void *dev_id)
{
        struct net_device *dev = (struct net_device *)dev_id;
        struct sunxi_can_priv *priv = netdev_priv(dev);
        struct net_device_stats *stats = &dev->stats;
        uint8_t isrc, status;
        int n = 0;

        while ((isrc = readl(CAN_INT_ADDR)) && (n < SUNXI_CAN_MAX_IRQ)) {
                n++;
                status = readl(CAN_STA_ADDR);
                /* check for absent controller due to hw unplug */
                if (sunxi_can_is_absent(priv))
                        return IRQ_NONE;

                if (isrc & WAKEUP)
                        netdev_warn(dev, "wakeup interrupt\n");

                if (isrc & TBUF_VLD) {
			pr_debug("sunxicanirq: Tx irq, reg=0x%X\n", isrc);
                        /* transmission complete interrupt */
                        stats->tx_bytes += readl(CAN_RBUF_RBACK_START_ADDR) & 0xf;
                        stats->tx_packets++;
                        can_get_echo_skb(dev, 0);
                        netif_wake_queue(dev);
                }
                if (isrc & RBUF_VLD) {
			pr_debug("sunxicanirq: Rx irq, reg=0x%X\n", isrc);
                        /* receive interrupt */
                        while (status & RBUF_RDY) {        //RX buffer is not empty
                                sunxi_can_rx(dev);
                                status = readl(CAN_STA_ADDR);
                                /* check for absent controller */
                                if (sunxi_can_is_absent(priv))
                                        return IRQ_NONE;
                        }
                }
                if (isrc & (DATA_ORUNI | ERR_WRN | BUS_ERR | ERR_PASSIVE | ARB_LOST)) {
			pr_debug("sunxicanirq: error, reg=0x%X\n", isrc);
                        /* error interrupt */
                        if (sunxi_can_err(dev, isrc, status))
                                break;
                }

                //clear the interrupt
                writel(isrc, CAN_INT_ADDR);
                readl(CAN_INT_ADDR);
        }

        if (n >= SUNXI_CAN_MAX_IRQ)
                netdev_dbg(dev, "%d messages handled in ISR", n);

        return (n) ? IRQ_HANDLED : IRQ_NONE;
}
EXPORT_SYMBOL_GPL(sunxi_can_interrupt);

static int sunxi_can_open(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);
        int err;

        /* set chip into reset mode */
        set_reset_mode(dev);

        writel(0xffffffff, CAN_ACPM_ADDR);

        /* common open */
        err = open_candev(dev);
        if (err)
                return err;

        /* register interrupt handler, if not done by the device driver */
        if (!(priv->flags & SUNXI_CAN_CUSTOM_IRQ_HANDLER)) {
                err = request_irq(dev->irq, sunxi_can_interrupt, priv->irq_flags,
                                 dev->name, (void *)dev);
                if (err) {
                        close_candev(dev);
                        pr_info("request_irq err:%d\n", err);
                        return -EAGAIN;
                }
        }

        /* init and start chi */
        sunxi_can_start(dev);
        priv->open_time = jiffies;

        netif_start_queue(dev);

        return 0;
}

static int sunxi_can_close(struct net_device *dev)
{
        struct sunxi_can_priv *priv = netdev_priv(dev);

        netif_stop_queue(dev);
        set_reset_mode(dev);

        if (!(priv->flags & SUNXI_CAN_CUSTOM_IRQ_HANDLER))
                free_irq(dev->irq, (void *)dev);

        close_candev(dev);

        priv->open_time = 0;

        return 0;
}

struct net_device *alloc_sunxicandev(int sizeof_priv)
{
        struct net_device *dev;
        struct sunxi_can_priv *priv;

        dev = alloc_candev(sizeof(struct sunxi_can_priv) + sizeof_priv,
                SUNXI_CAN_ECHO_SKB_MAX);
        if (!dev)
                return NULL;

        priv = netdev_priv(dev);

        priv->dev = dev;
        priv->can.bittiming_const = &sunxi_can_bittiming_const;
        priv->can.do_set_bittiming = sunxi_can_set_bittiming;
        priv->can.do_set_mode = sunxi_can_set_mode;
        priv->can.do_get_berr_counter = sunxi_can_get_berr_counter;
        priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
                CAN_CTRLMODE_LISTENONLY |
                CAN_CTRLMODE_3_SAMPLES |
                CAN_CTRLMODE_BERR_REPORTING;

        spin_lock_init(&priv->cmdreg_lock);

        if (sizeof_priv)
                priv->priv = (void *)priv + sizeof(struct sunxi_can_priv);

        return dev;
}
EXPORT_SYMBOL_GPL(alloc_sunxicandev);

void free_sunxicandev(struct net_device *dev)
{
        free_candev(dev);
}
EXPORT_SYMBOL_GPL(free_sunxicandev);

static const struct net_device_ops sunxican_netdev_ops = {
       .ndo_open = sunxi_can_open,
       .ndo_stop = sunxi_can_close,
       .ndo_start_xmit = sunxi_can_start_xmit,
};

int register_sunxicandev(struct net_device *dev)
{
        if (!sunxi_can_probe(dev))
                return -ENODEV;

        dev->flags |= IFF_ECHO;        /* support local echo */
        dev->netdev_ops = &sunxican_netdev_ops;

        set_reset_mode(dev);
        
        return register_candev(dev);
}
EXPORT_SYMBOL_GPL(register_sunxicandev);

void unregister_sunxicandev(struct net_device *dev)
{
        set_reset_mode(dev);
        unregister_candev(dev);
}
EXPORT_SYMBOL_GPL(unregister_sunxicandev);

static __init int sunxi_can_init(void)
{
        struct sunxi_can_priv *priv;
        int err = 0;
		int ret = 0;
		int used = 0;
		
        sunxican_dev = alloc_sunxicandev(0);
        if(!sunxican_dev) {
                pr_info("alloc sunxicandev fail\n");
        }
	
		ret = script_parser_fetch("can_para", "can_used", &used, sizeof (used));
		if ( ret || used == 0) {
			pr_info("[sunxi-can] Cannot setup CANBus driver, maybe not configured in script.bin?");
			goto exit_free;
		}
		
        priv = netdev_priv(sunxican_dev);
        sunxican_dev->irq = SW_INT_IRQNO_CAN;
        priv->irq_flags = 0;
        priv->can.clock.freq = clk_get_rate(clk_get(NULL, "can"));
        chipset_init(sunxican_dev);
        err = register_sunxicandev(sunxican_dev);
        if(err) {
                dev_err(&sunxican_dev->dev, "registering %s failed (err=%d)\n", DRV_NAME, err);
                goto exit_free;
        }

        dev_info(&sunxican_dev->dev, "%s device registered (reg_base=0x%08x, irq=%d)\n",
                 DRV_NAME, CAN_BASE0, sunxican_dev->irq);

        pr_info("%s CAN netdevice driver\n", DRV_NAME);

        return 0;

exit_free:
        free_sunxicandev(sunxican_dev);

        return err;
}
module_init(sunxi_can_init);

static __exit void sunxi_can_exit(void)
{
        unregister_sunxicandev(sunxican_dev);
        free_sunxicandev(sunxican_dev);

        pr_info("%s: driver removed\n", DRV_NAME);
}
module_exit(sunxi_can_exit);
