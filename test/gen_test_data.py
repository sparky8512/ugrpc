"""Generate test data for encode and decode of various protobuf types

 To run the tests, move the files this script generates onto a system
 'ugrpc' in PATH and then run test_xcode.sh on that system.
"""

import base64
import json

from google.protobuf import descriptor_pb2
from google.protobuf import descriptor_pool
from google.protobuf import message_factory

DECODE_PROTO3_TYPES = [
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_DOUBLE,
        "values": [ 1.0, float("-inf"), -1000000.0, 0.0, float("inf"), 1000000.0, float("nan") ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_FLOAT,
        "values": [ 1.0, float("-inf"), -1000000.0, 0.0, float("inf"), 1000000.0, float("nan") ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT64,
        "values": [ 1, -0x8000000000000000, -1000000, 0, 1000000, 0x7fffffffffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_UINT64,
        "values": [ 1, 0, 1000000, 0xffffffffffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT32,
        "values": [ 1, -0x80000000, -1000000, 0, 1000000, 0x7fffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_FIXED64,
        "values": [ 1, 0, 1000000, 0xffffffffffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_FIXED32,
        "values": [ 1, 0, 1000000, 0xffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_BOOL,
        "values": [ True, False ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_STRING,
        "values": [ "A value", "" ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_MESSAGE,
        "type_name": "Test008", # this is the INT32 non-repeated test msg
        "values": [ {"value": 1}, {} ],   # filled in later, once message class exists
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_BYTES,
        "values": [ b"A value", b"", b"\xff\xff\xff" ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_UINT32,
        "values": [ 1, 0, 1000000, 0xffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_ENUM,
        "type_name": "TestEnum",
        "values": [ "TEST_ONE", "TEST_UNSPECIFIED", 2 ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_SFIXED32,
        "values": [ 1, -0x80000000, -1000000, 0, 1000000, 0x7fffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_SFIXED64,
        "values": [ 1, -0x8000000000000000, -1000000, 0, 1000000, 0x7fffffffffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_SINT32,
        "values": [ 1, -0x80000000, -1000000, 0, 1000000, 0x7fffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_SINT64,
        "values": [ 1, -0x8000000000000000, -1000000, 0, 1000000, 0x7fffffffffffffff ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT64,
        "values": [ 1, 0 ],
        "packed": True,
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT64,
        "values": [ 1, 0 ],
        "packed": False,
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_FIXED64,
        "values": [ 1, 0 ],
        "packed": False,
    },
]

MULTIFIELD_PROTO3_TYPES = [
    {
        "field": "number",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT32,
        "value": 2,
    },
    {
        "field": "numbers",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT32,
        "repeat": True,
        "value": [3, 4, 5, 6],
    },
    {
        "field": "boolean",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_BOOL,
        "value": True,
    },
    {
        "field": "string",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_STRING,
        "value": "B value",
    },
    {
        "field": "enum",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_ENUM,
        "type_name": "EmbedEnum",
        "value": "EMBED_ONE",
    },
    {
        "field": "message",
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_MESSAGE,
        "type_name": "MultiTest",
        "value": {"a": 7, "b": 8},  # also set programmatically, so do not change
    },
]

DECODE_PROTO2_TYPES = [
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_UINT32,
        "values": [ 1, 0 ],
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT64,
        "values": [ 1, 0 ],
        "packed": False,
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_INT64,
        "values": [ 1, 0 ],
        "packed": True,
    },
    {
        "type": descriptor_pb2.FieldDescriptorProto.TYPE_FIXED64,
        "values": [ 1, 0 ],
        "packed": True,
    },
]


def serialize_bytes(obj):
    if isinstance(obj, bytes):
        return base64.b64encode(obj).decode("utf-8")
    raise TypeError


# NOTE: cat_msg, if set, is modified to replace it with merged msg
def write_gmsg_and_json(msg, jobj, i, j, direction, cat_msg=None):
    basename = "ugrpc.test.Test{:03}_{}_{:03}".format(i, direction, j)

    fgmsg = open(basename + ".gmsg", "wb")
    try:
        size = msg.ByteSize()
        if cat_msg:
            size += cat_msg.ByteSize()
        fgmsg.write(b"\x00" + size.to_bytes(4, "big"))
        fgmsg.write(msg.SerializeToString())
        if cat_msg:
            fgmsg.write(cat_msg.SerializeToString())
    finally:
        fgmsg.close()

    if cat_msg:
        fgmsg = open(basename + ".gmsg_merged", "wb")
        merge_str = msg.SerializeToString() + cat_msg.SerializeToString()
        cat_msg.ParseFromString(merge_str)

        try:
            fgmsg.write(b"\x00" + cat_msg.ByteSize().to_bytes(4, "big"))
            fgmsg.write(cat_msg.SerializeToString())
        finally:
            fgmsg.close()

    # 0 values, empty array, empty string all encode to empty object, so reflect
    # that in the decoded JSON
    if direction != "Enc" and "value" in jobj:
        value = jobj["value"]
        if not value or (isinstance(value, str) and value == "TEST_UNSPECIFIED"):
            jobj = {}

    fjson = open(basename + ".json", "w", newline="\n")
    try:
        json.dump(jobj, fjson, default=serialize_bytes, separators=(",", ":"))
        fjson.write("\n")
    finally:
        fjson.close()


def gen_proto3_tests(pool):
    fd_proto = descriptor_pb2.FileDescriptorProto()
    fd_proto.name = "tests_proto3"
    fd_proto.package = "ugrpc.test"
    fd_proto.syntax = "proto3"

    enm_desc = descriptor_pb2.EnumDescriptorProto()
    enm_desc.name = "TestEnum"

    val_desc = descriptor_pb2.EnumValueDescriptorProto()
    val_desc.name = "TEST_UNSPECIFIED"
    val_desc.number = 0
    enm_desc.value.append(val_desc)

    val_desc = descriptor_pb2.EnumValueDescriptorProto()
    val_desc.name = "TEST_ONE"
    val_desc.number = 1
    enm_desc.value.append(val_desc)

    fd_proto.enum_type.append(enm_desc)

    i = 0
    i_bytes_msg = None
    for info in DECODE_PROTO3_TYPES:
        for repeat in (False, True):
            fld_desc = descriptor_pb2.FieldDescriptorProto()
            fld_desc.name = "value"
            fld_desc.number = 1
            if repeat:
                fld_desc.label = descriptor_pb2.FieldDescriptorProto.LABEL_REPEATED
            elif info["type"] == descriptor_pb2.FieldDescriptorProto.TYPE_BYTES:
                i_bytes_msg = i
            fld_desc.type = info["type"]
            if "type_name" in info:
                fld_desc.type_name = info["type_name"]
            if "packed" in info:
                if not repeat:
                    continue
                fld_desc.options.packed = info["packed"]

            msg_desc = descriptor_pb2.DescriptorProto()
            msg_desc.name = "Test{:03}".format(i)
            i += 1
            msg_desc.field.append(fld_desc)

            fd_proto.message_type.append(msg_desc)

    for depth in (1, 5, 98):    # higher than 98 breaks the Python decode impl
        fld_desc = descriptor_pb2.FieldDescriptorProto()
        fld_desc.name = "value"
        fld_desc.number = 1
        fld_desc.type = descriptor_pb2.FieldDescriptorProto.TYPE_INT32

        nested_desc = None
        for j in range(depth):
            msg_desc = descriptor_pb2.DescriptorProto()
            name = "Nest{:03}".format(j)
            msg_desc.name = name
            msg_desc.field.append(fld_desc)

            if nested_desc:
                msg_desc.nested_type.append(nested_desc)
            nested_desc = msg_desc

            fld_desc = descriptor_pb2.FieldDescriptorProto()
            fld_desc.name = "nest"
            fld_desc.number = 1
            fld_desc.type = descriptor_pb2.FieldDescriptorProto.TYPE_MESSAGE
            fld_desc.type_name = name

        msg_desc = descriptor_pb2.DescriptorProto()
        msg_desc.name = "Test{:03}".format(i)
        i += 1
        msg_desc.field.append(fld_desc)
        msg_desc.nested_type.append(nested_desc)

        fd_proto.message_type.append(msg_desc)

    msg_desc = descriptor_pb2.DescriptorProto()
    msg_desc.name = "MultiTest"
    j = 1
    for field in ("a", "b"):
        fld_desc = descriptor_pb2.FieldDescriptorProto()
        fld_desc.name = field
        fld_desc.number = j
        j += 1
        fld_desc.type = descriptor_pb2.FieldDescriptorProto.TYPE_INT32

        msg_desc.field.append(fld_desc)

    fd_proto.message_type.append(msg_desc)

    msg_desc = descriptor_pb2.DescriptorProto()
    msg_desc.name = "Test{:03}".format(i)
    i += 1

    # test embedded enum
    enm_desc = descriptor_pb2.EnumDescriptorProto()
    enm_desc.name = "EmbedEnum"

    val_desc = descriptor_pb2.EnumValueDescriptorProto()
    val_desc.name = "EMBED_UNSPECIFIED"
    val_desc.number = 0
    enm_desc.value.append(val_desc)

    val_desc = descriptor_pb2.EnumValueDescriptorProto()
    val_desc.name = "EMBED_ONE"
    val_desc.number = 1
    enm_desc.value.append(val_desc)

    msg_desc.enum_type.append(enm_desc)

    j = 1
    for info in MULTIFIELD_PROTO3_TYPES:
        fld_desc = descriptor_pb2.FieldDescriptorProto()
        fld_desc.name = info["field"]
        fld_desc.number = j
        j += 1
        if "repeat" in info and info["repeat"]:
            fld_desc.label = descriptor_pb2.FieldDescriptorProto.LABEL_REPEATED
        fld_desc.type = info["type"]
        if "type_name" in info:
            fld_desc.type_name = info["type_name"]

        msg_desc.field.append(fld_desc)

    fd_proto.message_type.append(msg_desc)

    # for OneOf testing
    msg_desc = descriptor_pb2.DescriptorProto()
    msg_desc.name = "Test{:03}".format(i)
    i += 1

    msg_desc.enum_type.append(enm_desc)

    oof_desc = descriptor_pb2.OneofDescriptorProto()
    oof_desc.name = "oneof_test"
    msg_desc.oneof_decl.append(oof_desc)
    j = 1
    for info in MULTIFIELD_PROTO3_TYPES:
        if "repeat" in info and info["repeat"]:
            continue    # unwrapped repeated fields not allowed in OneOf
        fld_desc = descriptor_pb2.FieldDescriptorProto()
        fld_desc.name = info["field"]
        fld_desc.number = j
        j += 1
        fld_desc.type = info["type"]
        if "type_name" in info:
            fld_desc.type_name = info["type_name"]
        fld_desc.oneof_index = 0

        msg_desc.field.append(fld_desc)

    fd_proto.message_type.append(msg_desc)

    fd = pool.Add(fd_proto)

    i = 0
    for info in DECODE_PROTO3_TYPES:
        for repeat in (False, True):
            msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
            msg_class = message_factory.GetMessageClass(msg_type)

            if repeat:
                values = info["values"]
                msg_vals = values * int(16/len(values)) + values[0:(16%len(values))]
                msg = msg_class(value=msg_vals)
                write_gmsg_and_json(msg, {"value":msg_vals}, i, 0, "Dec")
                if info["type"] == descriptor_pb2.FieldDescriptorProto.TYPE_INT32:
                    for j, msg_vals in enumerate(([], [info["values"][0]] * 8192), start=1):
                        msg = msg_class(value=msg_vals)
                        write_gmsg_and_json(msg, {"value":msg_vals}, i, j, "Dec")
            elif "packed" not in info:
                for j, val in enumerate(info["values"]):
                    msg = msg_class(value=val)
                    write_gmsg_and_json(msg, {"value":val}, i, j, "Dec")
            else:
                continue
            i += 1

    for depth in (1, 5, 98):
        jobj = {"value": 1}
        for j in range(depth - 1):
            jobj = {"nest": jobj}
        msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
        msg_class = message_factory.GetMessageClass(msg_type)
        msg = msg_class(nest=jobj)
        write_gmsg_and_json(msg, {"nest": jobj}, i, 0, "Dec")
        i += 1

    msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
    msg_class = message_factory.GetMessageClass(msg_type)
    msg = msg_class()
    jobj = {}
    for info in MULTIFIELD_PROTO3_TYPES:
        if info["type"] == descriptor_pb2.FieldDescriptorProto.TYPE_MESSAGE:
            setattr(getattr(msg, info["field"]), "a", 7)
            setattr(getattr(msg, info["field"]), "b", 8)
        elif "repeat" in info and info["repeat"]:
            getattr(msg, info["field"]).extend(info["value"])
        else:
            setattr(msg, info["field"], info["value"])
        jobj[info["field"]] = info["value"]
    write_gmsg_and_json(msg, jobj, i, 0, "Dec")

    # test repeat of non-repeating scalar field
    alt_msg = msg_class(number=10)
    alt_jobj = jobj.copy()
    alt_jobj["number"] = 10
    write_gmsg_and_json(msg, alt_jobj, i, 1, "DecOnly", cat_msg=alt_msg)

    # test repeat of non-repeating message field
    alt_msg = msg_class(message={"b": 11})
    alt_jobj = jobj.copy()
    alt_jobj["message"]["b"] = 11
    write_gmsg_and_json(msg, alt_jobj, i, 2, "DecOnly", cat_msg=alt_msg)

    # test noncontiguous repeating field
    alt_msg = msg_class(numbers=[12, 13, 14])
    alt_jobj = jobj.copy()
    alt_jobj["numbers"].extend([12, 13, 14])
    write_gmsg_and_json(msg, alt_jobj, i, 3, "DecOnly", cat_msg=alt_msg)

    # test contiguous repeat of non-repeating scalar, low mem algo should be
    # able to handle this one
    msg = msg_class(number=15)
    alt_msg = msg_class(number=16)
    jobj = {"number": 16}
    write_gmsg_and_json(msg, jobj, i, 4, "DecOnly", cat_msg=alt_msg)

    i += 1

    msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
    msg_class = message_factory.GetMessageClass(msg_type)
    j = 0
    for info in MULTIFIELD_PROTO3_TYPES:
        if "repeat" in info and info["repeat"]:
            continue
        msg = msg_class()
        if info["type"] == descriptor_pb2.FieldDescriptorProto.TYPE_MESSAGE:
            setattr(getattr(msg, info["field"]), "a", 7)
            setattr(getattr(msg, info["field"]), "b", 8)
        else:
            setattr(msg, info["field"], info["value"])
        jobj = {info["field"]: info["value"]}
        write_gmsg_and_json(msg, jobj, i, j, "Dec")
        j += 1

    # test more than one OneOf
    alt_msg = msg_class(number=10)
    alt_jobj = {"number": 10}
    write_gmsg_and_json(msg, alt_jobj, i, 1, "DecOnly", cat_msg=alt_msg)

    # test duplicate OneOf (same field)
    alt_msg = msg_class(message={"b": 11})
    alt_jobj = jobj.copy()
    alt_jobj["message"]["b"] = 11
    write_gmsg_and_json(msg, alt_jobj, i, 2, "DecOnly", cat_msg=alt_msg)

    # test more than one OneOf, both scalars, so low mem algo should be able
    # to handle
    msg =  msg_class(number=17)
    alt_msg = msg_class(boolean=True)
    jobj = {"boolean": True}
    write_gmsg_and_json(msg, jobj, i, 3, "DecOnly", cat_msg=alt_msg)

    i += 1

    # Encode-only tests follow. Most of the encode testing is done reversing
    # decode tests. These are the encode tests that are not reversible:

    i = i_bytes_msg
    msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
    msg_class = message_factory.GetMessageClass(msg_type)
    bstr = b"\xfe\xdc\xba\x98\x76\x54\x32\x10"

    # unpadded base64
    msg = msg_class(value=bstr)
    write_gmsg_and_json(msg, {"value": base64.b64encode(bstr).decode("utf-8").rstrip("=")}, i, 0, "Enc")

    # URL-safe encoded base64
    msg = msg_class(value=bstr)
    write_gmsg_and_json(msg, {"value":base64.urlsafe_b64encode(bstr).decode("utf-8")}, i, 1, "Enc")

    i = 1
    msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
    msg_class = message_factory.GetMessageClass(msg_type)
    # explicitly empty repeated field
    msg = msg_class(value=[])
    write_gmsg_and_json(msg, {"value": []}, i, 0, "Enc")

    return fd_proto


def gen_proto2_tests(pool):
    fd_proto = descriptor_pb2.FileDescriptorProto()
    fd_proto.name = "tests_proto2"
    fd_proto.package = "ugrpc.test"
    fd_proto.syntax = "proto2"

    i = 100
    for info in DECODE_PROTO2_TYPES:
        fld_desc = descriptor_pb2.FieldDescriptorProto()
        fld_desc.name = "value"
        fld_desc.number = 1
        fld_desc.label = descriptor_pb2.FieldDescriptorProto.LABEL_REPEATED
        fld_desc.type = info["type"]
        if "packed" in info:
            fld_desc.options.packed = info["packed"]

        msg_desc = descriptor_pb2.DescriptorProto()
        msg_desc.name = "Test{:03}".format(i)
        i += 1
        msg_desc.field.append(fld_desc)

        fd_proto.message_type.append(msg_desc)

    fd = pool.Add(fd_proto)

    i = 100
    for info in DECODE_PROTO2_TYPES:
        msg_type = pool.FindMessageTypeByName("ugrpc.test.Test{:03}".format(i))
        msg_class = message_factory.GetMessageClass(msg_type)

        values = info["values"]
        msg_vals = values * int(16/len(values)) + values[0:(16%len(values))]
        msg = msg_class(value=msg_vals)
        write_gmsg_and_json(msg, {"value":msg_vals}, i, 0, "Dec")
        i += 1

    return fd_proto


def main():
    pool = descriptor_pool.DescriptorPool()
    fds_proto = descriptor_pb2.FileDescriptorSet()

    fd_proto3 = gen_proto3_tests(pool)
    f = open("test_proto3.fd", "wb")
    try:
        f.write(fd_proto3.SerializeToString())
    finally:
        f.close()
    fds_proto.file.append(fd_proto3)

    fd_proto2 = gen_proto2_tests(pool)
    f = open("test_proto2.fd", "wb")
    try:
        f.write(fd_proto2.SerializeToString())
    finally:
        f.close()
    fds_proto.file.append(fd_proto2)

    f = open("test_protos.fds", "wb")
    try:
        f.write(fds_proto.SerializeToString())
    finally:
        f.close()


if __name__ == "__main__":
    main()
