//! Barometric altitude with a per-minute history of the last 3 hours.
//!
//! Float types and operation order follow the C++ original exactly (f32 for
//! altitudes, f64 for the per-minute pressure sum) so results match bit for
//! bit; tests/altitude_golden.cpp checks that.

/// Minutes of history: 3 hours.
pub const MINUTES: usize = 180;

const HYSTERESIS_M: f32 = 3.0; // ~1 floor; BMP280 noise stays below it
const MAX_CAL_UNC_M: f32 = 15.0; // only trust fairly good vertical fixes

/// Standard atmosphere, sea level 1013.25 hPa. Absolute altitude is off by
/// whatever the weather is doing (tens of metres) until GPS calibrates it;
/// gain within the 3 h window is not affected either way.
pub fn pressure_to_altitude_m(hpa: f32) -> f32 {
    44330.0 * (1.0 - (hpa / 1013.25).powf(0.190295))
}

pub struct Altitude {
    cur_min: i64,
    sum_hpa: f64,
    count: i32,
    alt: [f32; MINUTES], // uncalibrated (standard-atmosphere) altitude
    tag: [i64; MINUTES], // which minute alt[i] belongs to; slot = minute % MINUTES
    have_live: bool,
    live_raw_m: f32,
    calibrated: bool,
    offset_m: f32,
}

impl Default for Altitude {
    fn default() -> Self {
        Altitude {
            cur_min: -1,
            sum_hpa: 0.0,
            count: 0,
            alt: [0.0; MINUTES],
            tag: [-1; MINUTES],
            have_live: false,
            live_raw_m: 0.0,
            calibrated: false,
            offset_m: 0.0,
        }
    }
}

impl Altitude {
    /// Adds one barometer sample. Samples are averaged per minute of
    /// CLOCK_BOOTTIME (batches arrive late, so the sample's own time decides
    /// its minute); the live value is the latest sample.
    pub fn add_pressure(&mut self, t_boot_s: f64, hpa: f32) {
        // Not `!(100.0..=1200.0).contains(..)`: that would also drop NaN,
        // which the C++ original let through.
        #[allow(clippy::manual_range_contains)]
        if hpa < 100.0 || hpa > 1200.0 {
            return; // not a plausible reading
        }
        let m = (t_boot_s / 60.0) as i64;
        if m < self.cur_min {
            return; // late sample for a closed minute
        }
        if m != self.cur_min {
            self.close_minute();
            self.cur_min = m;
        }
        self.sum_hpa += f64::from(hpa);
        self.count += 1;
        self.live_raw_m = pressure_to_altitude_m(hpa);
        self.have_live = true;
    }

    /// GPS altitude above sea level with its vertical uncertainty: sets the
    /// offset that maps standard-atmosphere altitude onto it (the weather
    /// moves sea-level pressure by tens of metres' worth). The whole history
    /// shifts together, so gain/loss over the window is unaffected.
    pub fn calibrate(&mut self, gps_msl_m: f32, vert_unc_m: f32) {
        if !self.have_live || vert_unc_m <= 0.0 || vert_unc_m > MAX_CAL_UNC_M {
            return;
        }
        let target = gps_msl_m - self.live_raw_m;
        if !self.calibrated {
            self.offset_m = target;
        } else {
            self.offset_m += 0.2 * (target - self.offset_m); // GPS altitude is noisy; ease in
        }
        self.calibrated = true;
    }

    pub fn calibrated(&self) -> bool {
        self.calibrated
    }

    pub fn valid(&self) -> bool {
        self.have_live
    }

    pub fn live_m(&self) -> f32 {
        self.live_raw_m + self.offset_m
    }

    /// Stores the finished minute's average in its ring slot.
    fn close_minute(&mut self) {
        if self.cur_min < 0 || self.count == 0 {
            return;
        }
        let slot = (self.cur_min % MINUTES as i64) as usize;
        self.alt[slot] = pressure_to_altitude_m((self.sum_hpa / f64::from(self.count)) as f32);
        self.tag[slot] = self.cur_min;
        self.sum_hpa = 0.0;
        self.count = 0;
    }

    /// Altitude of the minute `ago` minutes before the current one; None if
    /// there is no data for it.
    pub fn minute(&self, ago: i32) -> Option<f32> {
        if self.cur_min < 0 {
            return None;
        }
        let want = self.cur_min - i64::from(ago);
        if want < 0 {
            return None;
        }
        if ago == 0 {
            // the minute still being filled
            if self.count == 0 {
                return None;
            }
            return Some(
                pressure_to_altitude_m((self.sum_hpa / f64::from(self.count)) as f32)
                    + self.offset_m,
            );
        }
        let slot = (want % MINUTES as i64) as usize;
        if self.tag[slot] != want {
            return None;
        }
        Some(self.alt[slot] + self.offset_m)
    }

    /// Total climb / descent over the window, oldest to newest, counting a
    /// change only once it exceeds the hysteresis from the last turning
    /// point, so sensor noise doesn't pile up as fake ascent.
    pub fn totals(&self) -> (f32, f32) {
        let (mut up, mut down) = (0.0f32, 0.0f32);
        let mut reference: Option<f32> = None;
        for ago in (0..MINUTES as i32).rev() {
            let Some(a) = self.minute(ago) else { continue };
            match reference {
                None => reference = Some(a),
                Some(r) if a - r >= HYSTERESIS_M => {
                    up += a - r;
                    reference = Some(a);
                }
                Some(r) if r - a >= HYSTERESIS_M => {
                    down += r - a;
                    reference = Some(a);
                }
                Some(_) => {}
            }
        }
        (up, down)
    }
}

// --- C interface (altitude.h) ----------------------------------------------------

/// Opaque to C++: created by hb_alt_new, freed by hb_alt_free.
pub struct HbAltitude(Altitude);

/// Safety contract for every function here: `p` is a live pointer from
/// hb_alt_new (hb_alt_free ends it), out-pointers are valid, and calls come
/// from the one UI thread.
#[allow(clippy::missing_safety_doc)]
pub mod ffi {
    use super::{Altitude, HbAltitude};

    #[no_mangle]
    pub extern "C" fn hb_alt_new() -> *mut HbAltitude {
        Box::into_raw(Box::new(HbAltitude(Altitude::default())))
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_free(p: *mut HbAltitude) {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_add_pressure(p: *mut HbAltitude, t_boot_s: f64, hpa: f32) {
        (*p).0.add_pressure(t_boot_s, hpa);
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_calibrate(p: *mut HbAltitude, gps_msl_m: f32, vert_unc_m: f32) {
        (*p).0.calibrate(gps_msl_m, vert_unc_m);
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_calibrated(p: *const HbAltitude) -> bool {
        (*p).0.calibrated()
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_valid(p: *const HbAltitude) -> bool {
        (*p).0.valid()
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_live_m(p: *const HbAltitude) -> f32 {
        (*p).0.live_m()
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_minute(p: *const HbAltitude, ago: i32, m: *mut f32) -> bool {
        match (*p).0.minute(ago) {
            Some(v) => {
                *m = v;
                true
            }
            None => false,
        }
    }

    #[no_mangle]
    pub unsafe extern "C" fn hb_alt_totals(p: *const HbAltitude, up_m: *mut f32, down_m: *mut f32) {
        let (up, down) = (*p).0.totals();
        *up_m = up;
        *down_m = down;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn standard_atmosphere() {
        assert_eq!(pressure_to_altitude_m(1013.25), 0.0);
        let h = pressure_to_altitude_m(899.0); // ~1000 m
        assert!((h - 1000.0).abs() < 10.0, "{h}");
    }

    #[test]
    fn minutes_and_totals() {
        let mut a = Altitude::default();
        assert_eq!(a.minute(0), None);
        // 10 minutes climbing ~1 m/min, one sample per minute.
        for i in 0..10 {
            let hpa = 1000.0 - 0.12 * i as f32;
            a.add_pressure(60.0 * f64::from(i) + 1.0, hpa);
        }
        assert!(a.minute(0).is_some() && a.minute(9).is_some() && a.minute(10).is_none());
        let (up, down) = a.totals();
        assert!(up > 6.0 && down == 0.0, "{up} {down}");
    }

    #[test]
    fn calibration_needs_data_and_good_uncertainty() {
        let mut a = Altitude::default();
        a.calibrate(500.0, 5.0);
        assert!(!a.calibrated());
        a.add_pressure(1.0, 1013.25);
        a.calibrate(500.0, 20.0);
        assert!(!a.calibrated());
        a.calibrate(500.0, 5.0);
        assert!(a.calibrated() && a.live_m() == 500.0);
    }
}
