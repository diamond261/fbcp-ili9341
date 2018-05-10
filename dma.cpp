#ifndef KERNEL_MODULE
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <inttypes.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "config.h"
#include "dma.h"
#include "spi.h"
#include "util.h"

#ifdef USE_DMA_TRANSFERS

#define BCM2835_PERI_BASE               0x3F000000

SharedMemory *dmaSourceMemory = 0;
volatile DMAChannelRegisterFile *dma = 0;
volatile DMAChannelRegisterFile *dmaTx = 0;
volatile DMAChannelRegisterFile *dmaRx = 0;
int dmaTxChannel = 0;
int dmaTxIrq = 0;
int dmaRxChannel = 0;
int dmaRxIrq = 0;

#define PAGE_SIZE 4096

struct GpuMemory
{
  uint32_t allocationHandle;
  void *virtualAddr;
  uintptr_t busAddress;
  uint32_t sizeBytes;
};

#define NUM_DMA_CBS 512
GpuMemory dmaCb, dmaSourceBuffer;
GpuMemory dmaRecvCb;

volatile DMAControlBlock *dmaSendTail = 0;
//volatile DMAControlBlock *dmaRecvTail = 0;
volatile uint8_t *dmaSourceEnd = 0;

volatile DMAControlBlock *GrabFreeCBs(int num)
{
  volatile DMAControlBlock *firstCB = (volatile DMAControlBlock *)dmaCb.virtualAddr;
  volatile DMAControlBlock *endCB = firstCB + NUM_DMA_CBS;
  for(;;)
  {
//    volatile DMAControlBlock *firstFreeCB = MAX(firstCB, MAX(dmaSendTail+1, dmaRecvTail+1));
    volatile DMAControlBlock *firstFreeCB = MAX(firstCB, dmaSendTail+1);
    if (firstFreeCB + num <= endCB)
      return firstFreeCB;
    else
    {
//      printf("ops\n");
      WaitForDMAFinished();
      dmaSendTail = 0;
//      dmaRecvTail = 0;
    }
  }
}

volatile uint8_t *GrabFreeDMASourceBytes(int bytes)
{
  if ((uintptr_t)dmaSourceEnd + bytes > (uintptr_t)dmaSourceBuffer.virtualAddr + SHARED_MEMORY_SIZE)
    dmaSourceEnd = (volatile uint8_t *)dmaSourceBuffer.virtualAddr;

  volatile uint8_t *ret = dmaSourceEnd;
  dmaSourceEnd += bytes;
  return ret;
}

static int AllocateDMAChannel(int *dmaChannel, int *irq)
{
  // Snooping DMA, channels 3, 5 and 6 seen active.
  // TODO: Actually reserve the DMA channel to the system using bcm_dma_chan_alloc() and bcm_dma_chan_free()?...
  // Right now, use channels 1 and 4 which seem to be free.
  // Note: The send channel could be a lite channel, but receive channel cannot, since receiving uses the IGNORE flag
  // that lite DMA engines don't have.
  const int freeChannels[] = { 1, 7 };
  static int nextFreeChannel = 0;
  if (nextFreeChannel >= sizeof(freeChannels) / sizeof(freeChannels[0])) FATAL_ERROR("No free DMA channels");

  *dmaChannel = freeChannels[nextFreeChannel++];
  LOG("Allocated DMA channel %d", *dmaChannel);
  *irq = 0;
  return 0;
}

// Sends a pointer to the given buffer over to the VideoCore mailbox. See https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
void SendMailbox(void *buffer)
{
  int vcio = open("/dev/vcio", 0);
  if (vcio < 0) FATAL_ERROR("Failed to open VideoCore kernel mailbox!");
  int ret = ioctl(vcio, _IOWR(/*MAJOR_NUM=*/100, 0, char *), buffer);
  close(vcio);
  if (ret < 0) FATAL_ERROR("SendMailbox failed in ioctl!");
}

// Defines the structure of a Mailbox message
template<int PayloadSize>
struct MailboxMessage
{
  MailboxMessage(uint32_t messageId):messageSize(sizeof(*this)), requestCode(0), messageId(messageId), messageSizeBytes(sizeof(uint32_t)*PayloadSize), dataSizeBytes(sizeof(uint32_t)*PayloadSize), messageEndSentinel(0) {}
  uint32_t messageSize;
  uint32_t requestCode;
  uint32_t messageId;
  uint32_t messageSizeBytes;
  uint32_t dataSizeBytes;
  union
  {
    uint32_t payload[PayloadSize];
    uint32_t result;
  };
  uint32_t messageEndSentinel;
};

// Message IDs for different mailbox GPU memory allocation messages
#define MEM_ALLOC_MESSAGE 0x3000c // This message is 3 u32s: numBytes, alignment and flags
#define MEM_FREE_MESSAGE 0x3000f // This message is 1 u32: handle
#define MEM_LOCK_MESSAGE 0x3000d // 1 u32: handle
#define MEM_UNLOCK_MESSAGE 0x3000e // 1 u32: handle

// Memory allocation flags
#define MEM_ALLOC_FLAG_DIRECT (1 << 2) // Allocate uncached memory that bypasses L1 and L2 cache on loads and stores

// Sends a mailbox message with 1xuint32 payload
uint32_t Mailbox(uint32_t messageId, uint32_t payload0)
{
  MailboxMessage<1> msg(messageId);
  msg.payload[0] = payload0;
  SendMailbox(&msg);
  return msg.result;
}

// Sends a mailbox message with 3xuint32 payload
uint32_t Mailbox(uint32_t messageId, uint32_t payload0, uint32_t payload1, uint32_t payload2)
{
  MailboxMessage<3> msg(messageId);
  msg.payload[0] = payload0;
  msg.payload[1] = payload1;
  msg.payload[2] = payload2;
  SendMailbox(&msg);
  return msg.result;
}

#define BUS_TO_PHYS(x) ((x) & ~0xC0000000)

#define VIRT_TO_BUS(block, x) ((uintptr_t)(x) - (uintptr_t)((block).virtualAddr) + (block).busAddress)

// Allocates the given number of bytes in GPU side memory, and returns the virtual address and physical bus address of the allocated memory block.
// The virtual address holds an uncached view to the allocated memory, so writes and reads to that memory address bypass the L1 and L2 caches. Use
// this kind of memory to pass data blocks over to the DMA controller to process.
GpuMemory AllocateUncachedGpuMemory(uint32_t numBytes)
{
  GpuMemory mem;
  mem.sizeBytes = ALIGN_UP(numBytes, PAGE_SIZE);
  mem.allocationHandle = Mailbox(MEM_ALLOC_MESSAGE, /*size=*/mem.sizeBytes, /*alignment=*/PAGE_SIZE, /*flags=*/MEM_ALLOC_FLAG_DIRECT);
  mem.busAddress = Mailbox(MEM_LOCK_MESSAGE, mem.allocationHandle);
  mem.virtualAddr = mmap(0, mem.sizeBytes, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, BUS_TO_PHYS(mem.busAddress));
  if (mem.virtualAddr == MAP_FAILED) FATAL_ERROR("Failed to mmap GPU memory!");
  return mem;
}

void FreeUncachedGpuMemory(GpuMemory mem)
{
  munmap(mem.virtualAddr, mem.sizeBytes);
  Mailbox(MEM_UNLOCK_MESSAGE, mem.allocationHandle);
  Mailbox(MEM_FREE_MESSAGE, mem.allocationHandle);
}

int InitDMA()
{
#if defined(KERNEL_MODULE)
  dma = (volatile DMAChannelRegisterFile*)ioremap(BCM2835_PERI_BASE+BCM2835_DMA_BASE, 0x1000);
#else
  dma = (volatile DMAChannelRegisterFile*)((uintptr_t)bcm2835 + BCM2835_DMA_BASE);
#endif

#ifdef KERNEL_MODULE_CLIENT
  dmaTxChannel = spiTaskMemory->dmaTxChannel;
  dmaRxChannel = spiTaskMemory->dmaRxChannel;
#else
  int ret = AllocateDMAChannel(&dmaTxChannel, &dmaTxIrq);
  if (ret != 0) FATAL_ERROR("Unable to allocate TX DMA channel!");
  ret = AllocateDMAChannel(&dmaRxChannel, &dmaRxIrq);
  if (ret != 0) FATAL_ERROR("Unable to allocate RX DMA channel!");

  printf("Enabling DMA channels Tx:%d and Rx:%d\n", dmaTxChannel, dmaRxChannel);
  volatile uint32_t *dmaEnableRegister = (volatile uint32_t *)((uintptr_t)dma + BCM2835_DMAENABLE_REGISTER_OFFSET);

  // Enable the allocated DMA channels
  *dmaEnableRegister |= (1 << dmaTxChannel);
  *dmaEnableRegister |= (1 << dmaRxChannel);
#endif

#if !defined(KERNEL_MODULE)
  dmaCb = AllocateUncachedGpuMemory(sizeof(DMAControlBlock) * NUM_DMA_CBS);
  dmaRecvCb = AllocateUncachedGpuMemory(sizeof(DMAControlBlock));
  dmaSourceBuffer = AllocateUncachedGpuMemory(SHARED_MEMORY_SIZE);
  dmaSourceEnd = (volatile uint8_t *)dmaSourceBuffer.virtualAddr;
#endif

  LOG("DMA hardware register file is at ptr: %p, using DMA TX channel: %d and DMA RX channel: %d", dma, dmaTxChannel, dmaRxChannel);
  if (!dma) FATAL_ERROR("Failed to map DMA!");

  dmaTx = dma + dmaTxChannel;
  dmaRx = dma + dmaRxChannel;
  LOG("DMA hardware TX channel register file is at ptr: %p, DMA RX channel register file is at ptr: %p", dmaTx, dmaRx);

  // Reset the DMA channels
  LOG("Resetting DMA channels for use");
  dmaTx->cs = BCM2835_DMA_CS_RESET;
  dmaTx->cb.debug = BCM2835_DMA_DEBUG_DMA_READ_ERROR | BCM2835_DMA_DEBUG_DMA_FIFO_ERROR | BCM2835_DMA_DEBUG_READ_LAST_NOT_SET_ERROR;
  dmaRx->cs = BCM2835_DMA_CS_RESET;
  dmaRx->cb.debug = BCM2835_DMA_DEBUG_DMA_READ_ERROR | BCM2835_DMA_DEBUG_DMA_FIFO_ERROR | BCM2835_DMA_DEBUG_READ_LAST_NOT_SET_ERROR;

  // TODO: Set up IRQ
  LOG("DMA all set up");
  return 0;
}

// Debugging functions to introspect SPI and DMA hardware registers:

void DumpCS(uint32_t reg)
{
  PRINT_FLAG(BCM2835_DMA_CS_RESET);
  PRINT_FLAG(BCM2835_DMA_CS_ABORT);
  PRINT_FLAG(BCM2835_DMA_CS_DISDEBUG);
  PRINT_FLAG(BCM2835_DMA_CS_WAIT_FOR_OUTSTANDING_WRITES);
  PRINT_FLAG(BCM2835_DMA_CS_PANIC_PRIORITY);
  PRINT_FLAG(BCM2835_DMA_CS_PRIORITY);
  PRINT_FLAG(BCM2835_DMA_CS_ERROR);
  PRINT_FLAG(BCM2835_DMA_CS_WAITING_FOR_OUTSTANDING_WRITES);
  PRINT_FLAG(BCM2835_DMA_CS_DREQ_STOPS_DMA);
  PRINT_FLAG(BCM2835_DMA_CS_PAUSED);
  PRINT_FLAG(BCM2835_DMA_CS_DREQ);
  PRINT_FLAG(BCM2835_DMA_CS_INT);
  PRINT_FLAG(BCM2835_DMA_CS_END);
  PRINT_FLAG(BCM2835_DMA_CS_ACTIVE);
}

void DumpDebug(uint32_t reg)
{
  PRINT_FLAG(BCM2835_DMA_DEBUG_LITE);
  PRINT_FLAG(BCM2835_DMA_DEBUG_VERSION);
  PRINT_FLAG(BCM2835_DMA_DEBUG_DMA_STATE);
  PRINT_FLAG(BCM2835_DMA_DEBUG_DMA_ID);
  PRINT_FLAG(BCM2835_DMA_DEBUG_DMA_OUTSTANDING_WRITES);
  PRINT_FLAG(BCM2835_DMA_DEBUG_DMA_READ_ERROR);
  PRINT_FLAG(BCM2835_DMA_DEBUG_DMA_FIFO_ERROR);
  PRINT_FLAG(BCM2835_DMA_DEBUG_READ_LAST_NOT_SET_ERROR);
}

void DumpTI(uint32_t reg)
{
  PRINT_FLAG(BCM2835_DMA_TI_NO_WIDE_BURSTS);
  PRINT_FLAG(BCM2835_DMA_TI_WAITS);
  PRINT_FLAG(BCM2835_DMA_TI_PERMAP);
//  PRINT_FLAG(BCM2835_DMA_TI_BURST_LENGTH);
  PRINT_FLAG(BCM2835_DMA_TI_SRC_IGNORE);
  PRINT_FLAG(BCM2835_DMA_TI_SRC_DREQ);
  PRINT_FLAG(BCM2835_DMA_TI_SRC_WIDTH);
  PRINT_FLAG(BCM2835_DMA_TI_SRC_INC);
  PRINT_FLAG(BCM2835_DMA_TI_DEST_IGNORE);
  PRINT_FLAG(BCM2835_DMA_TI_DEST_DREQ);
  PRINT_FLAG(BCM2835_DMA_TI_DEST_WIDTH);
  PRINT_FLAG(BCM2835_DMA_TI_DEST_INC);
  PRINT_FLAG(BCM2835_DMA_TI_WAIT_RESP);
  PRINT_FLAG(BCM2835_DMA_TI_TDMODE);
  PRINT_FLAG(BCM2835_DMA_TI_INTEN);
}

#define DMA_SPI_CS_PHYS_ADDRESS 0x7E204000
#define DMA_SPI_FIFO_PHYS_ADDRESS 0x7E204004

#define DMA_GPIO_SET_PHYS_ADDRESS 0x7E20001C
#define DMA_GPIO_CLEAR_PHYS_ADDRESS 0x7E200028

void WaitForDMAFinished()
{
//  if ((dmaTx->cs & BCM2835_DMA_CS_ACTIVE) || (dmaRx->cs & BCM2835_DMA_CS_ACTIVE))
//  {
//    uint64_t t0 = tick();
//  printf("1\n");
  int spins = 0;
  uint64_t t0 = tick();
    while((dmaTx->cs & BCM2835_DMA_CS_ACTIVE))
    {
      usleep(100);
      if (tick() - t0 > 1000000)
      {
        printf("TX stalled\n");
        DumpCS(dmaTx->cs);
        DumpSPICS(spi->cs);
        DumpTI(dmaTx->cb.ti);
        printf("DMATX cbAddr: %p\n", dmaTx->cbAddr);
        exit(1);
      }
    }
//  printf("2\n");
    spins = 0;
    t0 = tick();
    while((dmaRx->cs & BCM2835_DMA_CS_ACTIVE))
    {
      usleep(100);
      if (tick() - t0 > 1000000)
      {
        printf("RX stalled\n");
        DumpCS(dmaRx->cs);
        DumpSPICS(spi->cs);
        DumpTI(dmaRx->cb.ti);
        printf("DMARX cbAddr: %p\n", dmaRx->cbAddr);
        exit(1);
      }
    }
//  printf("3\n");
//    uint64_t t1 = tick();
//    printf("Waited %llu usecs for dma\n", t1-t0);
//  }
    dmaSendTail = 0;
//    dmaRecvTail = 0;
}

void SPIDMATransfer(SPITask *task)
{
//  WaitForDMAFinished();
  // TODO: Ideally we would be able to directly perform the DMA from the SPI ring buffer from 'task' pointer. However
  // that pointer is shared to userland, and it is proving troublesome to make it both userland-writable as well as cache-bypassing DMA coherent.
  // Therefore these two memory areas are separate for now, and we memcpy() from SPI ring buffer to an intermediate 'dmaSourceMemory' memory area to perform
  // the DMA transfer. Is there a way to avoid this intermediate buffer? That would improve performance a bit.
  /*
  volatile uint32_t *buf = (volatile uint32_t *)dmaSourceBuffer.virtualAddr;
  volatile uint32_t *sendCmd = buf; */
  volatile uint32_t *sendCmd = (volatile uint32_t *)GrabFreeDMASourceBytes(8);
  sendCmd[0] = BCM2835_SPI0_CS_TA | (1 << 16); // The first four bytes written to the SPI data register control the DLEN and CS,CPOL,CPHA settings.
  sendCmd[1] = task->cmd;
  //volatile uint32_t *disableTA = buf+2;
  volatile uint32_t *disableTA = (volatile uint32_t *)GrabFreeDMASourceBytes(4);
  *disableTA = BCM2835_SPI0_CS_DMAEN | BCM2835_SPI0_CS_CLEAR_TX;
//  volatile uint32_t *set_gpio = buf+3;
  volatile uint32_t *set_gpio = (volatile uint32_t *)GrabFreeDMASourceBytes(4);
  *set_gpio = (1 << GPIO_TFT_DATA_CONTROL);
//  volatile uint32_t *sendPixels = buf+4;
  volatile uint32_t *sendPixels = (volatile uint32_t *)GrabFreeDMASourceBytes(4+task->size);
  sendPixels[0] = BCM2835_SPI0_CS_TA | (task->size << 16); // The first four bytes written to the SPI data register control the DLEN and CS,CPOL,CPHA settings.
  memcpy((void*)(sendPixels+1), (void*)&task->data, task->size);

  //volatile DMAControlBlock *cb = (volatile DMAControlBlock *)dmaCb.virtualAddr;
  volatile DMAControlBlock *cb = GrabFreeCBs(7);
//  printf("Got cbs at index %d\n", cb - (volatile DMAControlBlock *)dmaCb.virtualAddr);
  volatile DMAControlBlock *startSend = &cb[0];
  volatile DMAControlBlock *clear_dc_gpio_line = &cb[1];
  volatile DMAControlBlock *datacb = &cb[2];
  volatile DMAControlBlock *ta_disable = &cb[3];
  volatile DMAControlBlock *set_dc_gpio_line = &cb[4];
  volatile DMAControlBlock *txcb = &cb[5];
//  volatile DMAControlBlock *rxcb = &cb[6];
  volatile DMAControlBlock *rxcb = (volatile DMAControlBlock *)dmaRecvCb.virtualAddr;

  startSend->ti = BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_DEST_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS | BCM2835_DMA_TI_BURST_LENGTH(4);
  startSend->src = VIRT_TO_BUS(dmaSourceBuffer, disableTA);
  startSend->dst = DMA_SPI_CS_PHYS_ADDRESS; // Send SPI command
  startSend->len = sizeof(uint32_t);
  startSend->stride = 0;
  startSend->next = VIRT_TO_BUS(dmaCb, clear_dc_gpio_line);
  startSend->debug = 0;
  startSend->reserved = 0;

  clear_dc_gpio_line->ti = BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_DEST_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS | BCM2835_DMA_TI_BURST_LENGTH(4);
  clear_dc_gpio_line->src = VIRT_TO_BUS(dmaSourceBuffer, set_gpio);
  clear_dc_gpio_line->dst = DMA_GPIO_CLEAR_PHYS_ADDRESS; // Set GPIO pin low
  clear_dc_gpio_line->len = 4;
  clear_dc_gpio_line->stride = 0;
  clear_dc_gpio_line->next = VIRT_TO_BUS(dmaCb, datacb);
  clear_dc_gpio_line->debug = 0;
  clear_dc_gpio_line->reserved = 0;

  datacb->ti = BCM2835_DMA_TI_PERMAP_SPI_TX | BCM2835_DMA_TI_DEST_DREQ | BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS | BCM2835_DMA_TI_BURST_LENGTH(1);
  datacb->src = VIRT_TO_BUS(dmaSourceBuffer, sendCmd);
  datacb->dst = DMA_SPI_FIFO_PHYS_ADDRESS; // Send SPI command
  datacb->len = sizeof(uint8_t) + sizeof(uint32_t);
  datacb->stride = 0;
  datacb->next = VIRT_TO_BUS(dmaCb, ta_disable);
  datacb->debug = 0;
  datacb->reserved = 0;

  ta_disable->ti = BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_DEST_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS | BCM2835_DMA_TI_BURST_LENGTH(4);
  ta_disable->src = VIRT_TO_BUS(dmaSourceBuffer, disableTA);
  ta_disable->dst = DMA_SPI_CS_PHYS_ADDRESS; // Send SPI command
  ta_disable->len = sizeof(uint32_t);
  ta_disable->stride = 0;
  ta_disable->next = VIRT_TO_BUS(dmaCb, set_dc_gpio_line);
  ta_disable->debug = 0;
  ta_disable->reserved = 0;

  set_dc_gpio_line->ti = BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_DEST_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS | BCM2835_DMA_TI_BURST_LENGTH(4);
  set_dc_gpio_line->src = VIRT_TO_BUS(dmaSourceBuffer, set_gpio);
  set_dc_gpio_line->dst = DMA_GPIO_SET_PHYS_ADDRESS; // Set GPIO pin high
  set_dc_gpio_line->len = 4;
  set_dc_gpio_line->stride = 0;
  set_dc_gpio_line->next = VIRT_TO_BUS(dmaCb, txcb);
  set_dc_gpio_line->debug = 0;
  set_dc_gpio_line->reserved = 0;

  txcb->ti = BCM2835_DMA_TI_PERMAP_SPI_TX | BCM2835_DMA_TI_DEST_DREQ | BCM2835_DMA_TI_SRC_INC | BCM2835_DMA_TI_WAIT_RESP;// | BCM2835_DMA_TI_NO_WIDE_BURSTS;
  txcb->src = VIRT_TO_BUS(dmaSourceBuffer, sendPixels);
  txcb->dst = DMA_SPI_FIFO_PHYS_ADDRESS; // Write out to the SPI peripheral 
  txcb->len = task->size + sizeof(uint32_t);
  txcb->stride = 0;
  txcb->next = 0;
  txcb->debug = 0;
  txcb->reserved = 0;

  static uint64_t taskStartTime = 0;
  static int pendingTaskBytes = 1;
//  WaitForDMAFinished();
  const double spiSpeedUSecsPerByte = SPI_BUS_CLOCK_DIVISOR /*CDIV*/ * 8.0/*bits-to-bytes*/ / 400/*mbits/sec*/;
  double pendingTaskUSecs = pendingTaskBytes * spiSpeedUSecsPerByte;
  pendingTaskUSecs -= tick() - taskStartTime;
  if (pendingTaskUSecs > 100)
    usleep(MAX(pendingTaskUSecs-70, 0));

  while((dmaTx->cs & BCM2835_DMA_CS_ACTIVE))
    ;
  while((dmaRx->cs & BCM2835_DMA_CS_ACTIVE))
    ;
  dmaSendTail = 0;
  pendingTaskBytes = task->size;

  rxcb->ti = BCM2835_DMA_TI_PERMAP_SPI_RX | BCM2835_DMA_TI_SRC_DREQ | BCM2835_DMA_TI_DEST_IGNORE | BCM2835_DMA_TI_WAIT_RESP;
  rxcb->src = DMA_SPI_FIFO_PHYS_ADDRESS;
  rxcb->dst = 0;
  rxcb->len = 1 + task->size;
  rxcb->stride = 0;
  rxcb->next = 0;
  rxcb->debug = 0;
  rxcb->reserved = 0;

  __sync_synchronize();

  if (!dmaTx->cbAddr || !dmaSendTail)
  {
    dmaTx->cbAddr = VIRT_TO_BUS(dmaCb, startSend);
  }
  else
  {
    dmaSendTail->next = VIRT_TO_BUS(dmaCb, startSend);
  }
  dmaSendTail = txcb;

  dmaRx->cbAddr = VIRT_TO_BUS(dmaRecvCb, rxcb);
/*
  if (!dmaRx->cbAddr || !dmaRecvTail)
  {
    dmaRx->cbAddr = VIRT_TO_BUS(dmaCb, rxcb);
    printf("Started new RX\n");
  }
  else
  {
    dmaRecvTail->next = VIRT_TO_BUS(dmaCb, rxcb); 
    printf("Queued to existing RX\n");
  }
  dmaRecvTail = rxcb;
  */
/*
  if (!(dmaRx->cs & BCM2835_DMA_CS_ACTIVE))
  {
    dmaRx->cbAddr = VIRT_TO_BUS(dmaRecvCb, rxcb);
    dmaRx->cs = BCM2835_DMA_CS_ACTIVE | BCM2835_DMA_CS_WAIT_FOR_OUTSTANDING_WRITES;
  }
*/
  __sync_synchronize();
  dmaTx->cs = BCM2835_DMA_CS_ACTIVE;
  dmaRx->cs = BCM2835_DMA_CS_ACTIVE;
  __sync_synchronize();
  taskStartTime = tick();
}

#endif
