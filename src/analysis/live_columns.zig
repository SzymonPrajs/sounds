//! Shared output-column allocation for streaming analysis modes.

pub const Error = error{
    OutputFull,
};

pub fn append(columns: []f32, row_count: usize, capacity: usize, column_count: *usize) Error![]f32 {
    if (column_count.* >= capacity) return Error.OutputFull;

    const start = column_count.* * row_count;
    column_count.* += 1;
    return columns[start..][0..row_count];
}
