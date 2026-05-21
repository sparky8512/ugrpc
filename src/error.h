#ifndef _UGRPC_ERROR_H
#define _UGRPC_ERROR_H

//
// This is not meant to be a comprehensive list of possible errors, but
// rather just a few common ones that get reported in one place instead
// of printing the same error message all over.
//

// memory allocation error
#define ERROR_ALLOCATION -1

// invalid grpc or serialized protocol buffer detected and/or protocol
// data mismatch
#define ERROR_DECODE -2

// detailed error message already printed
#define ERROR_REPORTED -3

// these represent code defects, such as invalid internal state, and
// not user actionable
#define ERROR_INTERNAL(n) (ERROR_REPORTED - (n))
#define INTERNAL_ERROR_CODE(n) ERROR_INTERNAL(n)

#endif /* _UGRPC_ERROR_H */
