import os
import json
import math
import threading
import numpy as np


class UwbCalibrationManager:
    """Manages the 3-point UWB to UE coordinate transformation and IMU alignment."""

    def __init__(self, cache_path="calibration_cache.json"):
        self.lock = threading.Lock()
        self.cache_path = cache_path
        self.last_uwb_pos = None
        self.calib_points = []
        self.transform_matrix = None   # numpy 2×3 affine matrix
        self.imu_offset = 0.0          # Offset to align IMU yaw with UE world yaw
        self.is_heading_calibrated = False
        
        self.load_cache()

    def update_uwb_pos(self, x: float, y: float):
        """Update the latest raw UWB position tracked."""
        with self.lock:
            self.last_uwb_pos = (x, y)

    def add_calibration_point(self, ue_x: float, ue_y: float, point_index=None) -> tuple[int, str]:
        """
        Record current UE pos and latest UWB pos for one calibration slot.
        Returns: (valid_count, message)
        """
        with self.lock:
            if self.last_uwb_pos is None:
                return 0, "No UWB signal received yet. Please wait for UWB data."

            pair = {"ue": (ue_x, ue_y), "uwb": self.last_uwb_pos}

            if point_index is not None:
                idx = int(point_index)
                while len(self.calib_points) <= idx:
                    self.calib_points.append(None)
                self.calib_points[idx] = pair
                msg = f"Calib sample[{idx}] overwrite: UE({ue_x:.1f}, {ue_y:.1f}) ↔ UWB{self.last_uwb_pos}"
            else:
                if len(self.calib_points) >= 3:
                    # Auto-reset if all 3 slots are full
                    self.calib_points.clear()
                self.calib_points.append(pair)
                msg = f"Calib sample [{len(self.calib_points)}]: UE({ue_x:.1f}, {ue_y:.1f}) ↔ UWB{self.last_uwb_pos}"

        valid_count = sum(1 for p in self.calib_points if p is not None)
        return valid_count, msg

    def solve_transform(self) -> tuple[dict | None, str]:
        """
        Calculates the affine transformation matrix using all 3 calibration points
        (least-squares fit: UWB coords → UE coords).
        Returns: (matrix_info | None, message)
        """
        with self.lock:
            valid_pts = [p for p in self.calib_points if p is not None]
            if len(valid_pts) < 3:
                return None, f"Need 3 points to solve, got {len(valid_pts)}."

            # Build input / output arrays
            # uwb  [u, v]  →  ue  [x, y]
            # We solve:  [x]   [a b c] [u]
            #            [y] = [d e f] [v]
            #                          [1]
            uwb = np.array([[p["uwb"][0], p["uwb"][1], 1.0] for p in valid_pts], dtype=float)
            ue  = np.array([[p["ue"][0],  p["ue"][1]]       for p in valid_pts], dtype=float)

            # Least-squares: min ||uwb @ M.T - ue||
            result, _, _, _ = np.linalg.lstsq(uwb, ue, rcond=None)
            # result shape: (3, 2)  →  M = result.T  is (2, 3)
            M = result.T

            # Sanity: residual on training points
            ue_pred = uwb @ result       # (N, 2)
            residuals = np.sqrt(((ue_pred - ue) ** 2).sum(axis=1))
            max_err = float(residuals.max())

            self.transform_matrix = M
            self.save_cache()  # Persist after solving

            matrix_info = {
                "M": M.tolist(),
                "max_train_error_ue": max_err
            }
            return matrix_info, (
                f"Affine transform SOLVED from {len(valid_pts)} points. "
                f"Max training error: {max_err:.1f} UE units."
            )

    def calibrate_heading(self, current_imu_yaw: float, target_ue_yaw: float) -> float:
        """
        Calculate and store imu_offset = target_ue_yaw - current_imu_yaw.
        Returns the new offset.
        """
        with self.lock:
            # Normalize to 0-360
            offset = (target_ue_yaw - current_imu_yaw) % 360.0
            self.imu_offset = offset
            self.is_heading_calibrated = True
            self.save_cache()
            return offset

    def apply_imu_offset(self, raw_yaw: float) -> float:
        """Applies the stored offset to a raw IMU yaw reading."""
        with self.lock:
            return (raw_yaw + self.imu_offset) % 360.0

    def save_cache(self):
        """Save calibration data to a JSON file."""
        data = {
            "calib_points": self.calib_points,
            "imu_offset": self.imu_offset,
            "is_heading_calibrated": self.is_heading_calibrated,
            "transform_matrix": self.transform_matrix.tolist() if self.transform_matrix is not None else None
        }
        try:
            with open(self.cache_path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print(f"Error saving calibration cache: {e}")

    def load_cache(self):
        """Load calibration data from JSON file if it exists."""
        if not os.path.exists(self.cache_path):
            return
        try:
            with open(self.cache_path, "r", encoding="utf-8") as f:
                data = json.load(f)
                self.calib_points = data.get("calib_points", [])
                self.imu_offset = data.get("imu_offset", 0.0)
                self.is_heading_calibrated = data.get("is_heading_calibrated", False)
                m_list = data.get("transform_matrix")
                if m_list:
                    self.transform_matrix = np.array(m_list)
        except Exception as e:
            print(f"Error loading calibration cache: {e}")

    def clear(self):
        """Clears calibration state and deletes cache."""
        with self.lock:
            self.calib_points.clear()
            self.transform_matrix = None
            self.imu_offset = 0.0
            self.is_heading_calibrated = False
            if os.path.exists(self.cache_path):
                try:
                    os.remove(self.cache_path)
                except OSError:
                    pass

    def transform_uwb_to_ue(self, uwb_x: float, uwb_y: float) -> tuple[float, float] | None:
        """Apply the affine transform matrix to a UWB coordinate to obtain UE coordinate."""
        if self.transform_matrix is None:
            return None

        M = self.transform_matrix          # (2, 3)
        v = np.array([uwb_x, uwb_y, 1.0]) # (3,)
        result = M @ v                     # (2,)
        return (float(result[0]), float(result[1]))
