# FARGAN runtime from Opus

These files are copied from the Opus codec by the Xiph.Org Foundation
(https://github.com/xiph/opus). They are BSD-3-Clause licensed, see COPYING.
The upstream commit is in UPSTREAM_COMMIT. It is the same commit the released
model was trained with.

Only what FARGAN needs is here. Three files are changed on copy so that they
build without the rest of Opus:

* dnn/fargan.c - the LPCNet, PLC and pitch headers are not included, dnn/fargan_defs.h is included instead
* dnn/fargan.h - freq.h and pitchdnn.h are replaced by dnn/fargan_defs.h
* dnn/nnet.c - the DRED and PLC model headers are replaced by dnn/fargan_defs.h

dnn/fargan_defs.h is not part of Opus. It holds the few constants FARGAN takes
from the left out headers, with the same values as upstream.

To update from another Opus tree run

    bash third_party/opus/sync.sh /path/to/opus

and check that the model still matches with tests/compare_with_python.py.
Only update together with a model trained on the same Opus commit.
