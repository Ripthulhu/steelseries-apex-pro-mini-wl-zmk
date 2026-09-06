#!/usr/bin/env python3
"""Check keyboard sleep-entry guards with mocked Nordic registers."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[2] /
              'apex-zmk-g4b/src/sleep_g4b.c').read_text()
    function = source[source.index('void g4b_sleep_enter(void)'):]
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define IS_ENABLED(x) 1
#define BIT(x) (1u << (x))
#define __DSB() ((void)0)
#define __NOP() ((void)0)
#define G4B_MODE_DONGLE 2
#define G4B_SLEEP_MODE_PIN 3
#define G4B_PINCNF_SENSE_HIGH (2u << 16)
#define G4B_PINCNF_SENSE_LOW (3u << 16)
#define G4B_PINCNF_DIR_INPUT 0
#define G4B_PINCNF_INBUF_CONN 0
#define G4B_PINCNF_PULL_NONE 0
#define G4B_PORT0 0
#define G4B_P0_ATTN 24
#define POWER_USBREGSTATUS_VBUSDETECT_Msk 1
enum g4b_pin { PIN_DUMMY };
static struct { uint32_t PIN_CNF[32], LATCH, IN; } gpio;
static struct { uint32_t USBREGSTATUS, SYSTEMOFF; } power;
#define NRF_P0 (&gpio)
#define NRF_POWER (&power)
static int mode, pause_calls, resume_calls, irq_depth;
static bool pause_ok = true, attn;
static uint32_t sense;
static int g4b_mode_get(void) { return mode; }
static bool apex_radio_pause(void) { pause_calls++; return pause_ok; }
static void apex_radio_resume(void) { resume_calls++; }
static void g4b_rgb_set_blanked(bool value) { (void)value; }
static unsigned int irq_lock(void) { irq_depth++; return 7; }
static void irq_unlock(unsigned int key) { assert(key == 7); irq_depth--; }
static bool g4b_pin_read(int port, int pin) { (void)port; (void)pin; return attn; }
static void g4b_pin_cfg(int port, enum g4b_pin pin, uint32_t cfg) {
    (void)port; sense = cfg; gpio.PIN_CNF[pin] = cfg;
}
'''
    checks = r'''
int main(void) {
    mode = 2; gpio.IN = BIT(3); gpio.PIN_CNF[3] = 42;
    g4b_sleep_enter();
    assert(power.SYSTEMOFF == 1 && sense == G4B_PINCNF_SENSE_LOW);
    assert(pause_calls == 1 && resume_calls == 1 && irq_depth == 0);
    assert(gpio.PIN_CNF[3] == 42);
    power.SYSTEMOFF = 0; power.USBREGSTATUS = 1;
    g4b_sleep_enter(); assert(power.SYSTEMOFF == 0);
    power.USBREGSTATUS = 0; attn = true;
    g4b_sleep_enter(); assert(power.SYSTEMOFF == 0);
    attn = false; gpio.IN = 0;
    g4b_sleep_enter(); assert(power.SYSTEMOFF == 0);
    gpio.IN = BIT(3); pause_ok = false;
    int resumed = resume_calls;
    g4b_sleep_enter(); assert(power.SYSTEMOFF == 0 && resume_calls == resumed);
    mode = 0; gpio.IN = 0;
    g4b_sleep_enter();
    assert(power.SYSTEMOFF == 1 && sense == G4B_PINCNF_SENSE_HIGH);
    assert(irq_depth == 0 && gpio.PIN_CNF[3] == 42);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-sleep-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + function + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Sleep entry guard tests passed')


if __name__ == '__main__':
    main()
