#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace paper4 {
struct Point { int x = 0, y = 0; };
constexpr Point kCalibrationTargets[] = {{40,230}, {488,230}, {488,680}, {40,680}};
enum class CalibrationError { None, Duplicate, Degenerate, Invalid, Residual };
struct CalibrationReport {
    CalibrationError error = CalibrationError::None;
    int point = -1;
    int max_error = 0;
};
inline const char* CalibrationReason(CalibrationError error) {
    switch (error) {
    case CalibrationError::Duplicate: return "duplicate points";
    case CalibrationError::Degenerate: return "collinear or insufficient spread";
    case CalibrationError::Invalid: return "invalid transform";
    case CalibrationError::Residual: return "point residual exceeds 25 pixels";
    default: return "none";
    }
}
inline const char* CalibrationHint(CalibrationError error) {
    switch (error) {
    case CalibrationError::Duplicate: return "采点重复，请点不同的十字";
    case CalibrationError::Degenerate: return "采点过于集中或在同一直线上";
    case CalibrationError::Invalid: return "坐标变换无效";
    case CalibrationError::Residual: return "采点偏差较大，请按十字顺序点击";
    default: return "校准未通过";
    }
}
inline int RepeatedCalibrationPoint(const Point points[4], int count, Point next) {
    // Crosses are far apart even in the GT911's 320x480 raw space.
    for (int i = 0; i < count; ++i)
        if (std::hypot(double(points[i].x)-next.x,double(points[i].y)-next.y) < 16) return i;
    return -1;
}
struct TouchCalibration {
    // Version 1 required mandatory calibration and had no sensor-range identity.
    static constexpr uint32_t kVersion = 2;
    uint32_t version = 0;
    uint16_t raw_width = 0, raw_height = 0;
    float coefficient[2][3]{};
    bool Valid() const {
        if (version != kVersion) return false;
        for (const auto& row : coefficient) for (float value : row)
            if (!std::isfinite(value) || std::fabs(value) > 10000) return false;
        const float det = coefficient[0][0] * coefficient[1][1] - coefficient[0][1] * coefficient[1][0];
        return std::fabs(det) > 0.00001f;
    }
    bool Matches(int width, int height) const {
        return Valid() && width == raw_width && height == raw_height;
    }
    static TouchCalibration FromRange(int width, int height, bool reverse_x, bool reverse_y) {
        TouchCalibration result;
        if (width < 2 || height < 2 || width > 8192 || height > 8192) return result;
        result.version = kVersion;
        result.raw_width = uint16_t(width); result.raw_height = uint16_t(height);
        result.coefficient[0][0] = (reverse_x ? -527.0f : 527.0f) / (width-1);
        result.coefficient[1][1] = (reverse_y ? -791.0f : 791.0f) / (height-1);
        result.coefficient[0][2] = reverse_x ? 527.0f : 0.0f;
        result.coefficient[1][2] = reverse_y ? 791.0f : 0.0f;
        return result;
    }
    bool Map(Point raw, Point& result) const {
        if (!Valid()) return false;
        result.x = int(std::lround(coefficient[0][0]*raw.x + coefficient[0][1]*raw.y + coefficient[0][2]));
        result.y = int(std::lround(coefficient[1][0]*raw.x + coefficient[1][1]*raw.y + coefficient[1][2]));
        if (result.x < -8 || result.y < -8 || result.x > 535 || result.y > 799) return false;
        result.x = std::clamp(result.x, 0, 527); result.y = std::clamp(result.y, 0, 791);
        return true;
    }
    bool Fit(const Point raw[4], int width = 0, int height = 0, CalibrationReport* report = nullptr) {
        version = 0;
        if (report) *report = {};
        const auto reject = [&](CalibrationError error, int point = -1, int residual = 0) {
            version = 0;
            if (report) *report = {error,point,residual};
            return false;
        };
        for (int i = 1; i < 4; ++i)
            if (RepeatedCalibrationPoint(raw,i,raw[i]) >= 0) return reject(CalibrationError::Duplicate,i);
        // Least-squares affine fit supports swapped axes, rotation and mirrored sensor wiring.
        double system[3][5]{};
        for (int i = 0; i < 4; ++i) {
            const double input[] = {double(raw[i].x), double(raw[i].y), 1.0};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) system[row][col] += input[row] * input[col];
                system[row][3] += input[row] * kCalibrationTargets[i].x;
                system[row][4] += input[row] * kCalibrationTargets[i].y;
            }
        }
        for (int col = 0; col < 3; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 3; ++row)
                if (std::fabs(system[row][col]) > std::fabs(system[pivot][col])) pivot = row;
            if (std::fabs(system[pivot][col]) < 0.000001) return reject(CalibrationError::Degenerate);
            for (int c = 0; c < 5; ++c) std::swap(system[col][c], system[pivot][c]);
            const double scale = system[col][col];
            for (int c = 0; c < 5; ++c) system[col][c] /= scale;
            for (int row = 0; row < 3; ++row) if (row != col) {
                const double factor = system[row][col];
                for (int c = 0; c < 5; ++c) system[row][c] -= factor * system[col][c];
            }
        }
        for (int axis = 0; axis < 2; ++axis) for (int c = 0; c < 3; ++c)
            coefficient[axis][c] = float(system[c][3 + axis]);
        version = kVersion;
        raw_width = uint16_t(width); raw_height = uint16_t(height);
        if (!Valid()) return reject(CalibrationError::Invalid);
        for (int i = 0; i < 4; ++i) {
            Point mapped;
            if (!Map(raw[i], mapped)) return reject(CalibrationError::Residual,i);
            const int residual = std::max(std::abs(mapped.x-kCalibrationTargets[i].x),std::abs(mapped.y-kCalibrationTargets[i].y));
            if (residual > 25) return reject(CalibrationError::Residual,i,residual);
            if (report) report->max_error = std::max(report->max_error,residual);
        }
        return true;
    }
};
}
