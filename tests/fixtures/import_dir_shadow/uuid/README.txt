This folder exists on purpose. Its name matches the package "uuid", and it sits
next to check.kh. `import "uuid"` used to open this FOLDER as if it were the
module, and the compiler crashed (a multi-exabyte malloc). It must be skipped,
and the real packages/uuid/uuid.kh used instead.
