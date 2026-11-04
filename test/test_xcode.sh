#!/bin/sh
if [ $# -ne 1 ]; then
    echo "Usage: $0 <TEST_DATA_DIR>" >&2
    exit 1
fi

test_data_dir=$1

err=0

check_dec() {
    a=$1
    b=$2
    err_out=$3

    if cmp -s $a $b; then
        if [ -n "$err_out" ]; then
            if grep -q "WARNING" $test_data_dir/err.out; then
                echo "Unexpected WARNING issued on low-memory decode of test file ${i##*/}"
                err=1
                return 1
            fi
        fi
        return 0
    fi

    if [ -n "$err_out" ]; then
        if grep -q "WARNING" $test_data_dir/err.out; then
            return 0
        fi
        echo "No WARNING issued on low-memory decode compare failure"
    fi

    echo "Compare failure on decoded test file ${a##*/}:" >&2
    echo -n "  "
    cat $a
    echo "vs ${b##*/}:"
    echo -n "  "
    cat $b
    echo

    err=1
    return 1
}

check_enc() {
    a=$1
    b=$2
    prefix=$3

    if cmp -s $a $b; then
        return
    fi

    echo "Compare failure on ${prefix}encoded test file ${a##*/}:" >&2
    hexdump -C $a
    echo "vs ${b##*/}:"
    hexdump -C $b
    echo

    err=1
}

for i in $test_data_dir/ugrpc.test.Test*_Dec_*.gmsg; do
    basename=${i##*/}
    msgtype=${basename%%_*}
    # run this test using the .fd files and the rest using the combined .fds file
    if ! ugrpc -f $test_data_dir/test_proto3.fd -f $test_data_dir/test_proto2.fd decode $msgtype < $i > ${i%.gmsg}.json_dec 2>/dev/null; then
        echo "Error decoding test file ${i##*/}" >&2
        err=1
        continue
    fi

    if ! check_dec ${i%.gmsg}.json ${i%.gmsg}.json_dec; then
        continue
    fi

    # use eval in order to inject single quotes around the JSON string:
    if ! eval ugrpc -s $test_data_dir/test_protos.fds encode $msgtype "'"`cat ${i%.gmsg}.json_dec`"'" > ${i}_reenc 2>/dev/null; then
        echo "Error re-encoding test file ${i##*/}" >&2
        err=1
        continue
    fi

    check_enc $i ${i}_reenc "re-"
done

# repeat with low-mem algo
for i in $test_data_dir/ugrpc.test.Test*_Dec_*.gmsg; do
    basename=${i##*/}
    msgtype=${basename%%_*}
    if ! ugrpc -l -s $test_data_dir/test_protos.fds decode $msgtype < $i > ${i%.gmsg}.json_dec_ll 2>$test_data_dir/err.out; then
        echo "Error decoding test file ${i##*/} with low-memory algorithm" >&2
        err=1
        continue
    fi

    check_dec ${i%.gmsg}.json ${i%.gmsg}.json_dec_ll $test_data_dir/err.out
done

for i in $test_data_dir/ugrpc.test.Test*_DecOnly_*.gmsg; do
    basename=${i##*/}
    msgtype=${basename%%_*}
    if ! ugrpc -s $test_data_dir/test_protos.fds decode $msgtype < $i > ${i%.gmsg}.json_dec 2>/dev/null; then
        echo "Error decoding test file ${i##*/}" >&2
        err=1
        continue
    fi

    check_dec ${i%.gmsg}.json ${i%.gmsg}.json_dec
done

# repeat with low-mem algo
for i in $test_data_dir/ugrpc.test.Test*_DecOnly_*.gmsg; do
    basename=${i##*/}
    msgtype=${basename%%_*}
    if ! ugrpc -l -s $test_data_dir/test_protos.fds decode $msgtype < $i > ${i%.gmsg}.json_dec_ll 2>$test_data_dir/err.out; then
        echo "Error decoding test file ${i##*/} with low-memory algorithm" >&2
        err=1
        continue
    fi

    check_dec ${i%.gmsg}.json ${i%.gmsg}.json_dec_ll $test_data_dir/err.out
done

for i in $test_data_dir/ugrpc.test.Test*_Enc_*.json; do
    basename=${i##*/}
    msgtype=${basename%%_*}
    if ! eval ugrpc -s $test_data_dir/test_protos.fds encode $msgtype "'"`cat ${i}`"'" > ${i%.json}.gmsg_enc 2>/dev/null; then
        echo "Error encoding test file ${i##*/}" >&2
        err=1
        continue
    fi

    check_enc ${i%.json}.gmsg ${i%.json}.gmsg_enc ""
done

exit $err
