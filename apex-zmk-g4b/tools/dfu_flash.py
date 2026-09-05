#!/usr/bin/env python3
"""Trigger APEXBOOT DFU on the dfu_cdc port and copy a UF2 to it.
Usage: dfu_flash.py <uf2-path> [dfu-com-port]"""
import serial, time, os, sys, shutil, string

DFU_PORT = sys.argv[2] if len(sys.argv) > 2 else "COM12"
UF2 = sys.argv[1]

def touch_1200(port):
    try:
        s = serial.Serial(port, 1200)
        try:
            s.dtr = False
        except Exception:
            pass
        time.sleep(0.2)
        s.close()
        print(f"1200-baud touch on {port}")
        return True
    except Exception as e:
        print("1200 touch failed:", e)
        return False

def magic(port):
    try:
        s = serial.Serial(port, 115200)
        s.write(b"APEXDFU!")
        s.flush()
        time.sleep(0.15)
        s.close()
        print(f"wrote APEXDFU! to {port}")
        return True
    except Exception as e:
        print("magic write failed:", e)
        return False

def find_boot_drive():
    for L in string.ascii_uppercase:
        root = f"{L}:\\"
        try:
            if os.path.exists(root + "INFO_UF2.TXT") or os.path.exists(root + "CURRENT.UF2"):
                return root
        except Exception:
            pass
    return None

def wait_drive(timeout_s):
    for _ in range(timeout_s):
        d = find_boot_drive()
        if d:
            return d
        time.sleep(1)
    return None

def main():
    if not os.path.isfile(UF2):
        print("UF2 not found:", UF2); sys.exit(2)
    d = find_boot_drive()
    if not d:
        touch_1200(DFU_PORT)
        d = wait_drive(20)
    if not d:
        magic(DFU_PORT)
        d = wait_drive(15)
    if not d:
        print("\nAPEXBOOT drive did not appear. Enter DFU manually")
        print("(hold Fn + Right-Ctrl + Esc, or double-tap reset) then re-run.")
        sys.exit(1)
    print("APEXBOOT mounted at", d)
    try:
        print("--- INFO_UF2.TXT ---\n" + open(d + "INFO_UF2.TXT").read())
    except Exception:
        pass
    dest = d + "NEW.UF2"
    print(f"copying {UF2} -> {dest}")
    # copyfile (not copy): the UF2 bootloader flashes and unmounts the drive the
    # instant the file lands, so a follow-up chmod (shutil.copy) throws WinError
    # 433. The byte copy itself is what triggers the flash.
    try:
        shutil.copyfile(UF2, dest)
    except OSError as e:
        print(f"(expected on flash+unmount: {e})")
    print("UF2 written; the board should flash and reboot into the new firmware.")

if __name__ == "__main__":
    main()
