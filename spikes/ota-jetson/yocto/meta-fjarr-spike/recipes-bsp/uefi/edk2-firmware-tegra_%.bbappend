# spec: docs/32-fleet-ota.md#jetson-test-image
# The images and the capsule take TEGRA_BOOTCONTROL_OVERLAYS from DEPLOY_DIR_IMAGE after this
# recipe's deploy, as for meta-tegra's own RootfsRedundancyLevel overlay.
do_deploy[depends] += "fjarr-l4t-config:do_deploy"
