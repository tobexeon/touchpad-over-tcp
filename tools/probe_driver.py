import ctypes, struct, sys, time

# MTP_IOCTL_STATUS: ABI 4 + submit 4 + laststatus 4 + reportsize 2 +
# active 1 + inputmode 1 + functionswitch 1 + reserved 3 + get 4 + set 4 = 28
STATUS_FMT = '<IIiHBBB3xII'
DEVICE = r'\\.\MtpVhfTouchpad'
IOCTL_MTP_QUERY_STATUS = (0x8000 << 16) | (0x0001 << 14) | (0x801 << 2) | 0

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

def query():
    handle = kernel32.CreateFileW(
        DEVICE,
        0x80000000 | 0x40000000,  # GENERIC_READ | GENERIC_WRITE
        0, None, 3, 0x80, None,   # OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL
    )
    if handle == ctypes.c_void_p(-1).value or handle == -1:
        print('CreateFile failed:', ctypes.get_last_error())
        return None
    buf = ctypes.create_string_buffer(28)
    returned = ctypes.c_ulong(0)
    ok = kernel32.DeviceIoControl(
        ctypes.c_void_p(handle), IOCTL_MTP_QUERY_STATUS,
        None, 0, buf, 28, ctypes.byref(returned), None,
    )
    if not ok:
        print('DeviceIoControl failed:', ctypes.get_last_error())
        kernel32.CloseHandle(ctypes.c_void_p(handle))
        return None
    kernel32.CloseHandle(ctypes.c_void_p(handle))
    vals = struct.unpack(STATUS_FMT, buf.raw[:28])
    return {
        'abi': vals[0], 'submits': vals[1], 'last_ntstatus': hex(vals[2] & 0xffffffff),
        'report_bytes': vals[3], 'active': vals[4], 'input_mode': vals[5],
        'function_switch': hex(vals[6]), 'get_feature': vals[7], 'set_feature': vals[8],
    }

if __name__ == '__main__':
    for i in range(3):
        s = query()
        print(i, s)
        if i < 2:
            time.sleep(2)
