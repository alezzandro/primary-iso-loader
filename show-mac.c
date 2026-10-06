/* EFI helper: print the virtio-net MAC and stop.
 * Used when the discovery ISO is not attached. GRUB chainloads this
 * from the primary boot partition. No libc.
 */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

struct text_output {
    void *reset;
    uint64_t (*output_string)(struct text_output *this, uint16_t *string)
        __attribute__((ms_abi));
};

struct system_table {
    char header[24];
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t pad0;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    struct text_output *con_out;
    void *standard_error_handle;
    void *std_err;
    void *runtime_services;
    void *boot_services;
};

struct boot_services {
    char header[24];
    void *raise_tpl;
    void *restore_tpl;
    void *allocate_pages;
    void *free_pages;
    void *get_memory_map;
    void *allocate_pool;
    void *free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    void *handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    void *exit_boot_services;
    void *get_next_monotonic_count;
    void *stall;
    uint64_t (*set_watchdog_timer)(uint64_t timeout, uint64_t code,
                                   uint64_t data_size, uint16_t *data)
        __attribute__((ms_abi));
};

static void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(0xcf8, 0x80000000u | ((uint32_t) bus << 16) | ((uint32_t) dev << 11) |
                    ((uint32_t) fn << 8) | (off & 0xfcu));
    return inl(0xcfc);
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (uint8_t) (pci_read32(bus, dev, fn, off) >> ((off & 3) * 8));
}

static void pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t value)
{
    outl(0xcf8, 0x80000000u | ((uint32_t) bus << 16) | ((uint32_t) dev << 11) |
                    ((uint32_t) fn << 8) | (off & 0xfcu));
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"((uint16_t) (0xcfc + (off & 3))));
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t value)
{
    outl(0xcf8, 0x80000000u | ((uint32_t) bus << 16) | ((uint32_t) dev << 11) |
                    ((uint32_t) fn << 8) | (off & 0xfcu));
    outl(0xcfc, value);
}

static void print(struct text_output *out, const char *text)
{
    uint16_t wide[160];
    int i = 0;
    while (text[i] != 0 && i < 158) {
        wide[i] = (uint8_t) text[i];
        i++;
    }
    wide[i] = 0;
    out->output_string(out, wide);
}

static char hex_digit(uint8_t nibble)
{
    return (char) (nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

static int mac_is_zero(const uint8_t mac[6])
{
    return mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 &&
           mac[4] == 0 && mac[5] == 0;
}

static void print_mac(struct text_output *out, const uint8_t mac[6])
{
    char line[40];
    int i;
    int p = 0;
    const char *prefix = "MAC address: ";
    while (prefix[p] != 0) {
        line[p] = prefix[p];
        p++;
    }
    for (i = 0; i < 6; i++) {
        if (i != 0)
            line[p++] = ':';
        line[p++] = hex_digit(mac[i] >> 4);
        line[p++] = hex_digit(mac[i] & 0x0f);
    }
    line[p++] = '\r';
    line[p++] = '\n';
    line[p] = 0;
    print(out, line);
}

static uint64_t pci_bar_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar)
{
    uint32_t low = pci_read32(bus, dev, fn, (uint8_t) (0x10 + bar * 4));
    if ((low & 0x1u) != 0)
        return 0;
    if ((low & 0x6u) == 0x4u) {
        uint32_t high = pci_read32(bus, dev, fn, (uint8_t) (0x10 + (bar + 1) * 4));
        return ((uint64_t) high << 32) | (low & ~0xfu);
    }
    return low & ~0xfu;
}

static int read_modern_mac(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t mac[6])
{
    uint8_t cap = pci_read8(bus, dev, fn, 0x34);
    uint8_t dev_bar = 0;
    uint32_t dev_off = 0;
    uint8_t window = 0;
    int have_dev = 0;
    int guard = 0;

    while (cap >= 0x40 && guard < 48) {
        uint8_t id = pci_read8(bus, dev, fn, cap);
        uint8_t next = pci_read8(bus, dev, fn, (uint8_t) (cap + 1));
        if (id == 0x09) {
            uint8_t cfg_type = pci_read8(bus, dev, fn, (uint8_t) (cap + 3));
            if (cfg_type == 4) {
                dev_bar = pci_read8(bus, dev, fn, (uint8_t) (cap + 4));
                dev_off = pci_read32(bus, dev, fn, (uint8_t) (cap + 8));
                have_dev = 1;
            } else if (cfg_type == 5) {
                window = cap;
            }
        }
        if (next == 0)
            break;
        cap = next;
        guard++;
    }
    if (!have_dev)
        return 0;

    /* Virtio PCI configuration window. It works when the firmware has
     * not mapped the memory BAR. */
    if (window != 0) {
        uint32_t word;
        pci_write8(bus, dev, fn, (uint8_t) (window + 4), dev_bar);
        pci_write32(bus, dev, fn, (uint8_t) (window + 8), dev_off);
        pci_write32(bus, dev, fn, (uint8_t) (window + 12), 4);
        word = pci_read32(bus, dev, fn, (uint8_t) (window + 16));
        mac[0] = (uint8_t) word;
        mac[1] = (uint8_t) (word >> 8);
        mac[2] = (uint8_t) (word >> 16);
        mac[3] = (uint8_t) (word >> 24);
        pci_write32(bus, dev, fn, (uint8_t) (window + 8), dev_off + 4);
        pci_write32(bus, dev, fn, (uint8_t) (window + 12), 2);
        word = pci_read32(bus, dev, fn, (uint8_t) (window + 16));
        mac[4] = (uint8_t) word;
        mac[5] = (uint8_t) (word >> 8);
        if (!mac_is_zero(mac))
            return 1;
    }

    if (dev_bar <= 5) {
        uint64_t base = pci_bar_addr(bus, dev, fn, dev_bar);
        volatile uint8_t *cfg;
        int i;
        if (base == 0)
            return 0;
        cfg = (volatile uint8_t *) (base + dev_off);
        for (i = 0; i < 6; i++)
            mac[i] = cfg[i];
        return !mac_is_zero(mac);
    }
    return 0;
}

static int read_legacy_mac(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t mac[6])
{
    uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
    uint16_t port;
    int i;
    if ((bar0 & 0x1u) == 0)
        return 0;
    port = (uint16_t) (bar0 & ~0x3u);
    for (i = 0; i < 6; i++)
        mac[i] = inb((uint16_t) (port + 20 + i));
    return !mac_is_zero(mac);
}

static int virtio_net(uint32_t vendor_device)
{
    uint16_t vendor = (uint16_t) vendor_device;
    uint16_t device = (uint16_t) (vendor_device >> 16);
    /* Transitional virtio-net, and modern virtio-net (0x1040 + 1). */
    return vendor == 0x1af4 && (device == 0x1000 || device == 0x1041);
}

void efi_main(void *image, struct system_table *table)
{
    struct text_output *out = table->con_out;
    struct boot_services *bs = table->boot_services;
    int found = 0;
    int bus;
    (void) image;

    if (bs != 0 && bs->set_watchdog_timer != 0)
        bs->set_watchdog_timer(0, 0, 0, 0);

    print(out, "No discovery or installation ISO found.\r\n");
    print(out, "Attach the ISO and reboot.\r\n");

    for (bus = 0; bus < 256; bus++) {
        int dev;
        for (dev = 0; dev < 32; dev++) {
            int fn;
            int header = pci_read8((uint8_t) bus, (uint8_t) dev, 0, 0x0e);
            int nfn = (header & 0x80) ? 8 : 1;
            uint32_t id0 = pci_read32((uint8_t) bus, (uint8_t) dev, 0, 0);
            if ((uint16_t) id0 == 0xffff)
                continue;
            for (fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read32((uint8_t) bus, (uint8_t) dev, (uint8_t) fn, 0);
                uint8_t mac[6];
                if ((uint16_t) id == 0xffff)
                    continue;
                if (!virtio_net(id))
                    continue;
                if (read_legacy_mac((uint8_t) bus, (uint8_t) dev, (uint8_t) fn, mac) ||
                    read_modern_mac((uint8_t) bus, (uint8_t) dev, (uint8_t) fn, mac)) {
                    print_mac(out, mac);
                    found = 1;
                }
            }
        }
    }

    if (!found)
        print(out, "No virtio network card found.\r\n");

    for (;;)
        __asm__ volatile("hlt");
}

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  subq $40, %rsp\n"
    "  movq %rcx, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  call efi_main\n"
    "  addq $40, %rsp\n"
    "  ret\n"
    ".section .reloc,\"a\"\n"
    ".balign 4\n"
    ".long 0\n"
    ".long 10\n"
    ".text\n"
);
