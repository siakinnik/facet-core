// The kernel's messages on the text console (dmesg -n): Facet draws on the
// framebuffer under that console, and a flood of driver errors (a failing USB
// device) printed there at 4K stalls the whole machine for seconds. While
// Facet runs only the severe ones (below `level`) are printed; the journal and
// dmesg still get everything. The old level comes back when Facet ends.
#pragma once

#include <string>

namespace facet::platform {

class ConsoleLog {
public:
    // `printk`: the kernel's file with the four levels; `saved`: where the
    // level before us is kept so that a run that was killed cannot leave the
    // console quiet for good (the next run puts it back).
    explicit ConsoleLog(std::string printk = "/proc/sys/kernel/printk",
                        std::string saved = "/run/facet/printk.orig")
        : printk_(std::move(printk)), saved_(std::move(saved)) {}
    ~ConsoleLog() { restore(); }
    ConsoleLog(const ConsoleLog&) = delete;
    ConsoleLog& operator=(const ConsoleLog&) = delete;

    // Needs root; without it nothing changes. False if nothing was changed.
    bool quiet(int level = 3);
    void restore();
    bool active() const { return active_; }

private:
    std::string printk_, saved_;
    bool active_ = false;
    int original_ = 0, level_ = 0;
};

}  // namespace facet::platform
