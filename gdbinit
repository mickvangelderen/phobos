define connect
target extended-remote localhost:3333
monitor version
set remote hardware-breakpoint-limit 6
set remote hardware-watchpoint-limit 4
load
monitor reset init
end

define hook-step
mon cortex_m maskisr on
end

define hookpost-step
mon cortex_m maskisr off
end

define reload
make
monitor reset halt
load
monitor reset halt
end

tui enable

set trace-commands on
set logging on
set print pretty

python
import os
import sys
sys.path.insert(0, os.path.join(os.path.expanduser('~'), 'repos/phobos/debug'))
from printers import register_eigen_printers
register_eigen_printers(None)
end

source /Users/oliver/repos/phobos/external/gdb-regview/gdb-regview.py
regview load /Users/oliver/repos/phobos/external/gdb-regview/defs/stm32f40x.xml
