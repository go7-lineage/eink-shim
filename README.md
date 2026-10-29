# eink-shim

E-Ink support shim and QS tile for the Boox Go 7 on LineageOS

## Using in a device tree

1. Check it out next to the device, such as in a local manifest:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
    <project name="go7-lineage/eink-shim" path="hardware/onyx/eink" remote="github" revision="master" />
    ...
</manifest>
```

2. In `device.mk`, `PRODUCT_PACKAGES += libeink_shim`
3. In vendor props, `vendor.display.primary_mixer_stages=1` so every frame is one fullscreen plane
4. In `BoardConfig.mk`, `BOARD_VENDOR_SEPOLICY_DIRS += hardware/onyx/eink/sepolicy`. Device policy must provide `ebc_device` for `/dev/ebc`
5. In `extract-files.py`, make composer load the shim first so that its libdrm symbols win:
   ```python
   'vendor/bin/hw/vendor.qti.hardware.display.composer-service': blob_fixup()
       .add_needed('libeink_shim.so'),
   ```
