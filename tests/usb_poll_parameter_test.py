#!/usr/bin/env python3
"""Runtime boundary checks for the poll_us module parameter (no USB I/O)."""
import errno
from pathlib import Path
p=Path('/sys/module/usbwarp/parameters/poll_us')
original=p.read_text().strip()
checks=0
try:
    for value in ('1','10','1000','0'):
        p.write_text(value)
        assert p.read_text().strip()==value
        checks+=1
    for value in ('1001','4294967295','-1','invalid'):
        try:
            p.write_text(value)
        except OSError as e:
            assert e.errno in (errno.EINVAL,errno.ERANGE)
        else:
            raise AssertionError('Invalid interval accepted: '+value)
        assert p.read_text().strip()=='0'
        checks+=1
finally:
    p.write_text(original)
print(f'poll_us parameter: {checks} boundary checks passed; original value restored')
