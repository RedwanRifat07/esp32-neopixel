import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import env_to_sdkconfig as e  # noqa: E402

VALID = {
    "WIFI_SSID": "lab", "WIFI_PASSWORD": 'p"w\\d-long', "IMS_URL": "https://ims.local/x",
    "IMS_TOKEN": "tok", "DEVICE_ID": "esp-1", "LED_COUNT": "12",
}


class Tests(unittest.TestCase):
    def test_second_strip(self):
        out = e.build_config(dict(VALID, LED_COUNT="5", LED2_COUNT="4"))
        self.assertIn("CONFIG_SIOT_LED_COUNT=5", out)
        self.assertIn("CONFIG_SIOT_LED2_COUNT=4", out)
        self.assertIn("CONFIG_SIOT_LED2_GPIO=19", out)

    def test_second_strip_defaults_off(self):
        self.assertIn("CONFIG_SIOT_LED2_COUNT=0", e.build_config(VALID))

    def test_second_strip_same_gpio_rejected(self):
        with self.assertRaises(e.EnvError):
            e.build_config(dict(VALID, LED2_COUNT="4", LED2_GPIO="18"))

    def test_total_over_30_rejected(self):
        with self.assertRaises(e.EnvError):
            e.build_config(dict(VALID, LED_COUNT="20", LED2_COUNT="11"))

    def test_valid_defaults(self):
        out = e.build_config(VALID)
        self.assertIn("CONFIG_SIOT_LED_GPIO=18", out)
        self.assertIn("CONFIG_SIOT_LED_COUNT=12", out)
        self.assertIn("CONFIG_SIOT_GLOW_RGB=0xFFB46E", out)
        self.assertIn(r'CONFIG_SIOT_WIFI_PASSWORD="p\"w\\d-long"', out)

    def test_missing_and_placeholder_rejected(self):
        bad = dict(VALID, IMS_TOKEN="<ims-api-token>")
        del bad["LED_COUNT"]
        with self.assertRaises(e.EnvError) as c:
            e.build_config(bad)
        msg = str(c.exception)
        self.assertIn("IMS_TOKEN", msg)
        self.assertIn("LED_COUNT", msg)
        self.assertNotIn("<ims-api-token>", msg)

    def test_secrets_not_in_error_output(self):
        with self.assertRaises(e.EnvError) as c:
            e.build_config(dict(VALID, LED_COUNT="99"))
        self.assertNotIn("tok", str(c.exception))

    def test_forbidden_gpio_and_ranges(self):
        for k, v in (("LED_GPIO", "12"), ("LED_COUNT", "31"), ("GLOW_BRIGHTNESS_PCT", "0"),
                     ("POLL_INTERVAL_MS", "10"), ("GLOW_RGB", "XYZ")):
            with self.assertRaises(e.EnvError, msg=k):
                e.build_config(dict(VALID, **{k: v}))

    def test_ims_and_web_token_optional(self):
        minimal = {"WIFI_SSID": "lab", "WIFI_PASSWORD": "longenough", "LED_COUNT": "9"}
        out = e.build_config(minimal)
        self.assertIn("CONFIG_SIOT_LED_COUNT=9", out)
        self.assertNotIn("SIOT_IMS", out)
        self.assertNotIn("SIOT_WEB_TOKEN", out)
        self.assertIn('CONFIG_SIOT_WEB_TOKEN="secret-token"',
                      e.build_config(dict(minimal, WEB_TOKEN="secret-token")))

    def test_optional_placeholder_still_rejected(self):
        with self.assertRaises(e.EnvError):
            e.build_config(dict(VALID, IMS_URL="<https://ims.example/api/...>"))

    def test_length_and_charset_limits(self):
        for k, v in (("WIFI_SSID", "x" * 33), ("WIFI_PASSWORD", "short"),
                     ("WIFI_PASSWORD", "x" * 64), ("WEB_TOKEN", "abc"),
                     ("WEB_TOKEN", "has space1"), ("WEB_TOKEN", "x" * 65)):
            with self.assertRaises(e.EnvError, msg=f"{k}={len(v)}"):
                e.build_config(dict(VALID, **{k: v}))

    def test_url_scheme(self):
        with self.assertRaises(e.EnvError):
            e.build_config(dict(VALID, IMS_URL="ftp://x"))

    def test_parse_quotes_comments_export(self):
        v = e.parse_env('# c\nexport A="x y"\nB=\'z\'\n\nC=1=2\n')
        self.assertEqual(v, {"A": "x y", "B": "z", "C": "1=2"})

    def test_main_missing_file_and_perms(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(e.main(["--env", f"{d}/nope", "--out", f"{d}/o"]), 1)
            env = f"{d}/.env"
            with open(env, "w") as f:
                f.write("\n".join(f"{k}={v}" for k, v in VALID.items() if k != "WIFI_PASSWORD")
                        + "\nWIFI_PASSWORD=abcdefgh\n")
            self.assertEqual(e.main(["--env", env, "--out", f"{d}/o"]), 0)
            self.assertEqual(os.stat(f"{d}/o").st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
