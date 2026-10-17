# spec: docs/32-fleet-ota.md#jetson-test-image
# The release the slot reports, as the x86 images do: IMAGE_ID and IMAGE_VERSION, which the
# applier compares with what it installed.
OS_RELEASE_FIELDS:append:fjarr-spike = " IMAGE_ID IMAGE_VERSION"
OS_RELEASE_UNQUOTED_FIELDS:append:fjarr-spike = " IMAGE_ID IMAGE_VERSION"
IMAGE_ID:fjarr-spike = "fjarr"
IMAGE_VERSION:fjarr-spike = "${FJARR_IMAGE_VERSION}"
