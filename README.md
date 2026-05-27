# CCL_RLE AVX Test Bench

This folder contains an x64 MFC test harness for the supplied `CCL_RLE.cpp/.h` implementation.

## Build

1. Open `CCL_RLE_TEST.sln` in Visual Studio 2022 or newer.
2. Select `x64` and use `Release` for timing measurements.
3. Build and run. The supplied JPG loads automatically when it can be found from the executable directory.

The detector uses AVX2 instructions and therefore requires an AVX2-capable machine.

## Test Flow

- `Open Image...` loads an image through WIC and converts it to an 8-bit grayscale buffer.
- The supplied sample opens in `Use 6 Fixed ROIs` mode and runs lanes `L0` through `L5` in parallel.
- `Use Whole Image` provides a one-job baseline. Dragging on the image creates a custom single ROI.
- Defaults mirror the reported Coat invocation: `Black or white`, black threshold `10`, white threshold `130`, scale `0.042 x 0.028`, `isWhiteImage=false`, 8-connectivity, and `500` returned candidates per ROI.
- `Black only` models the reported Null mode while preserving the current ROI layout.
- `Run Once` shows one timing sample. `Benchmark` applies optional warm-up and reports averages across the requested repeats.
- The wide bottom table is the timing result table. The smaller left table lists returned blobs.

## Timing Columns

Each timing row represents one ROI worker. For `Benchmark`, columns show the average duration across measured repetitions.

- `Worker Total`: Complete time within that ROI worker.
- `Buffer`: Allocation of the per-ROI debug buffer retained from the supplied batch path.
- `Column Corr.`: `BuildColCorrPrecomp`, including column means and correction offsets.
- `Thresh/Runs`: AVX2 thresholding and run emission for every row.
- `Label Merge`: Union-find merging between adjacent rows.
- `Statistics`: Per-component moments/statistics and row interval collection.
- `Features`: Area sort, selected blob feature generation, perimeter and compactness.
- `Out Merge`: Locked insertion of the ROI result vector into the public batch result.
- `Returned`: Candidate count returned for that ROI after filtering and the per-ROI limit.

`Batch timing` is measured around the complete measured parallel batch call. It includes job dispatch and waiting until all ROI workers complete. Image decoding, UI painting, and table updates are outside the measurement.

## Integration Notes

- The supplied algorithm files remain unchanged. The measured runner in `CCL_RLE_UnicodeAdapter.cpp` follows the same calculation sequence with timing boundaries inserted between stages.
- `gThreadPool.h` is a minimal local implementation of the job pool dependency referenced by the supplied header.
- The reported production call supplies the same `gGetImgPtr()` pointer as both white and dark input while using `isWhiteImage=false`. This harness supplies its one loaded grayscale buffer in the same manner.
- `SetMaxDefectCnt(500)` is applied per ROI by the algorithm, so six ROI jobs may return up to `3000` public candidates.

## Fixed ROI Coordinates

Paste production ROI coordinates into `kFixedRoiPreset` near the top of `MainFrame.cpp`:

```cpp
constexpr std::array<FixedRoiDefinition, 6> kFixedRoiPreset =
{{
    { left0, top0, right0, bottom0 }, // Lane 0
    { left1, top1, right1, bottom1 }, // Lane 1
    { left2, top2, right2, bottom2 }, // Lane 2
    { left3, top3, right3, bottom3 }, // Lane 3
    { left4, top4, right4, bottom4 }, // Lane 4
    { left5, top5, right5, bottom5 }  // Lane 5
}};
```

All six rectangles must be valid and inside the loaded image. If any rectangle is invalid, the application uses a temporary six-way equal-width layout.
