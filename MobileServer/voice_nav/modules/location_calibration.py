import math
import threading

class UwbCalibrationManager:
    """Manages the 2-point UWB to UE coordinate transformation."""
    
    def __init__(self):
        self.lock = threading.Lock()
        self.last_uwb_pos = None
        self.calib_points = []
        self.transform_matrix = None

    def update_uwb_pos(self, x: float, y: float):
        """Update the latest raw UWB position tracked."""
        with self.lock:
            self.last_uwb_pos = (x, y)

    def add_calibration_point(self, ue_x: float, ue_y: float, point_index=None) -> tuple[int, str]:
        """
        Record current UE pos and latest UWB pos.
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
                if len(self.calib_points) >= 2:
                    # Auto-reset if array is full
                    self.calib_points.clear()
                self.calib_points.append(pair)
                msg = f"Calib sample [{len(self.calib_points)}]: UE({ue_x:.1f}, {ue_y:.1f}) ↔ UWB{self.last_uwb_pos}"

        valid_count = sum(1 for p in self.calib_points if p is not None)
        return valid_count, msg

    def solve_transform(self) -> tuple[dict | None, str]:
        """
        Calculates the transformation matrix.
        Returns: (matrix | None, message)
        """
        with self.lock:
            valid_pts = [p for p in self.calib_points if p is not None]
            if len(valid_pts) < 2:
                return None, f"Need 2 points to solve, got {len(valid_pts)}."

            ue_pts = (valid_pts[0]["ue"], valid_pts[1]["ue"])
            uwb_pts = (valid_pts[0]["uwb"], valid_pts[1]["uwb"])

            matrix = self._solve_2point_transform(ue_pts, uwb_pts)
            if matrix:
                self.transform_matrix = matrix
                return matrix, f"Transformation SOLVED: Scale={matrix['S']:.3f}, Theta={matrix['theta']:.3f} rad"
            else:
                return None, "Math error (UWB points too close? Move to a different location for each capture.)"

    def clear(self):
        """Clears calibration state."""
        with self.lock:
            self.calib_points.clear()
            self.transform_matrix = None

    def transform_uwb_to_ue(self, uwb_x: float, uwb_y: float) -> tuple[float, float] | None:
        """Apply transform matrix to a UWB coordinate to get UE coordinate."""
        if not self.transform_matrix:
            return None
            
        e1_x, e1_y = self.transform_matrix["E1"]
        w1_x, w1_y = self.transform_matrix["W1"]
        s = self.transform_matrix["S"]
        theta = self.transform_matrix["theta"]

        dx = uwb_x - w1_x
        dy = uwb_y - w1_y

        rx = dx * math.cos(theta) - dy * math.sin(theta)
        ry = dx * math.sin(theta) + dy * math.cos(theta)

        return (e1_x + s * rx, e1_y + s * ry)

    def _solve_2point_transform(self, ue_points, uwb_points):
        (ue1_x, ue1_y), (ue2_x, ue2_y) = ue_points
        (uwb1_x, uwb1_y), (uwb2_x, uwb2_y) = uwb_points

        d_ue_x = ue2_x - ue1_x
        d_ue_y = ue2_y - ue1_y
        d_uwb_x = uwb2_x - uwb1_x
        d_uwb_y = uwb2_y - uwb1_y

        dist_ue = math.hypot(d_ue_x, d_ue_y)
        dist_uwb = math.hypot(d_uwb_x, d_uwb_y)

        if dist_uwb < 0.001:
            return None

        scale = dist_ue / dist_uwb
        angle_ue = math.atan2(d_ue_y, d_ue_x)
        angle_uwb = math.atan2(d_uwb_y, d_uwb_x)
        theta = angle_ue - angle_uwb

        return {"E1": (ue1_x, ue1_y), "W1": (uwb1_x, uwb1_y), "S": scale, "theta": theta}
