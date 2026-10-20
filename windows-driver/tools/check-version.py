#!/usr/bin/env python3
"""Version gate for the in-repo Windows driver build (no device access)."""
import argparse
import ctypes
import json
import re
import sys
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]  # windows-driver/


def expected_version():
    text = (ROOT / 'include' / 'nnpi_version.h').read_text()
    value = re.search(r'^#define NNPI_DRIVER_VERSION "(\d+\.\d+\.\d+\.\d+)"$', text, re.M).group(1)
    numeric = re.search(r'^#define NNPI_DRIVER_VERSION_NUM ([\d,]+)$', text, re.M).group(1)
    if numeric.replace(',', '.') != value:
        raise ValueError('Version header numeric/string mismatch')
    return value


def sys_versions(path):
    api = ctypes.WinDLL('version', use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    size = api.GetFileVersionInfoSizeW(str(Path(path).resolve()), None)
    if not size:
        raise ValueError('SYS missing PE version resource')
    data = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(Path(path).resolve()), 0, size, data):
        raise ValueError('Cannot read SYS version resource')

    def query(name):
        ptr, length = ctypes.c_void_p(), wintypes.UINT()
        if not api.VerQueryValueW(data, name, ctypes.byref(ptr), ctypes.byref(length)) or not length.value:
            raise ValueError('Missing version field: ' + name)
        return ptr, length.value

    ptr, length = query('\\')
    if length < 52:
        raise ValueError('Truncated fixed version')
    fields = ctypes.cast(ptr, ctypes.POINTER(wintypes.DWORD))
    if fields[0] != 0xFEEF04BD:
        raise ValueError('Invalid fixed version signature')

    def version(ms, ls):
        return '.'.join(map(str, (ms >> 16, ms & 65535, ls >> 16, ls & 65535)))

    result = {'fixed_file': version(fields[2], fields[3]), 'fixed_product': version(fields[4], fields[5])}
    ptr, length = query('\\VarFileInfo\\Translation')
    if length % 4 or not length:
        raise ValueError('Invalid resource translations')
    words = ctypes.cast(ptr, ctypes.POINTER(wintypes.WORD))
    for i in range(0, length // 2, 2):
        for field in ('FileVersion', 'ProductVersion'):
            key = '\\StringFileInfo\\%04x%04x\\%s' % (words[i], words[i + 1], field)
            p, n = query(key)
            result[key] = ctypes.wstring_at(p, n).rstrip('\0')
    return result


def check(inf, sys_path, expected=None):
    expected = expected or expected_version()
    raw = Path(inf).read_bytes()
    text = raw.decode('utf-16') if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else raw.decode('utf-8-sig')
    versions = re.findall(r'^\s*DriverVer\s*=\s*[^,;\r\n]+,\s*([\d.]+)\s*(?:;[^\r\n]*)?$', text, re.I | re.M)
    if versions != [expected]:
        raise ValueError('Actual INF DriverVer mismatch: ' + repr(versions))
    values = sys_versions(sys_path)
    if not values or any(v != expected for v in values.values()):
        raise ValueError('Actual SYS FileVersion/ProductVersion mismatch: ' + repr(values))
    return {'inf_version': versions[0], 'sys_versions': values}


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--project', action='store_true')
    p.add_argument('--inf')
    p.add_argument('--sys')
    p.add_argument('--expected')
    args = p.parse_args()
    if args.project:
        expected_version()
    if args.inf or args.sys:
        if args.expected and args.expected != expected_version():
            raise ValueError('Receipt/candidate/source version mismatch')
        print(json.dumps(check(args.inf, args.sys, args.expected), indent=2))