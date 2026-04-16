## 2024-04-16 - Prevent OOB Access on SCSI Responses
**Vulnerability:** Out-of-bounds (OOB) memory reads/writes occurred when handling SCSI/ATAPI command responses (e.g., INQUIRY, MODE SENSE). The code implicitly trusted the device to return standard-length buffers, directly accessing `result.Data[index]` without validating the `result.Data.size()`.
**Learning:** Hardware devices (especially failing or malicious peripherals) can return truncated responses. Treating external IO boundaries as trusted data sources without length validation is a critical security gap leading to crashes or memory corruption.
**Prevention:** Always validate `Data.size() > Required_Index` before accessing elements in variable-length buffers returned from external IO calls, even low-level hardware interfaces.
