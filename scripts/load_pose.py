#!/usr/bin/env python
# -*- coding: utf-8 -*-
import os
import sys
import numpy as np

from phobos import load
from phobos import pose
from phobos import pb


"""
if __name__ == '__main__':
    _, dtype, desc = pose.parse_format()
    if len(sys.argv) < 2:
        print('Usage: {} <pose_log_file>\n\nLoad pose data'.format(__file__))
        print('    <pose_log_file>\tFile containing samples in ' +
              'the given packed struct binary format:\n')
        print(desc)
        sys.exit(1)

    filename = os.path.realpath(sys.argv[1])
    gitsha1, data, num_errors = load.pose_log(filename, dtype)
    print('firmware version {0}'.format(gitsha1))
    print('read {0} total packets, {1} decode errors'.format(
        len(data), num_errors))

    sys.exit(0)

"""

if __name__ == '__main__':
    proto_files = ['../projects/proto/pose.proto',
                   '../projects/proto/simulation.proto']
    proto = pb.import_modules(proto_files)[-1]

    def deserialize_callback(packet):
        return pb.decode_delimited(proto.SimulationMessage(), packet)

    messages = load.cobs_framed_log(sys.argv[1], deserialize_callback, True)
    print('got {} message(s)'.format(len(messages)))
