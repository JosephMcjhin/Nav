import math
import threading
import numpy as np


class UwbCalibrationManager:
    """Manages the 3-point UWB to UE coordinate transformation (affine, least-squares)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.last_uwb_pos = None
        self.calib_points = []
        self.transform_matrix = None   # numpy 2×3 affine matrix

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
            matrix_info = {
                "M": M.tolist(),
                "max_train_error_ue": max_err
            }
            return matrix_info, (
                f"Affine transform SOLVED from {len(valid_pts)} points. "
                f"Max training error: {max_err:.1f} UE units."
            )

    def clear(self):
        """Clears calibration state."""
        with self.lock:
            self.calib_points.clear()
            self.transform_matrix = None

    def transform_uwb_to_ue(self, uwb_x: float, uwb_y: float) -> tuple[float, float] | None:
        """Apply the affine transform matrix to a UWB coordinate to obtain UE coordinate."""
        if self.transform_matrix is None:
            return None

        M = self.transform_matrix          # (2, 3)
        v = np.array([uwb_x, uwb_y, 1.0]) # (3,)
        result = M @ v                     # (2,)
        return (float(result[0]), float(result[1]))
