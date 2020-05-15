/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test that image preview tooltip shows updated content when the image src
// changes.

/*eslint-disable */
const INITIAL_SRC = "data:image/png;base64,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";
/* eslint-enable */

const UPDATED_SRC = URL_ROOT + "doc_markup_tooltip.png";

const INITIAL_SRC_SIZE = "64" + " \u00D7 " + "64";
const UPDATED_SRC_SIZE = "22" + " \u00D7 " + "23";

add_task(async function() {
  const { inspector } = await openInspectorForURL(
    "data:text/html,<p>markup view tooltip test</p><img>"
  );

  info("Retrieving NodeFront for the <img> element.");
  const img = await getNodeFront("img", inspector);

  info("Selecting the <img> element");
  await selectNode(img, inspector);

  info("Adding src attribute to the image.");
  await updateImageSrc(img, INITIAL_SRC, inspector);

  const container = getContainerForNodeFront(img, inspector);
  ok(container, "Found markup container for the image.");

  let target = container.editor
    .getAttributeElement("src")
    .querySelector(".link");
  ok(target, "Found the src attribute in the markup view.");

  info("Showing tooltip on the src link.");
  await assertTooltipShownOnHover(inspector.markup.imagePreviewTooltip, target);

  checkImageTooltip(INITIAL_SRC_SIZE, inspector);

  await assertTooltipHiddenOnMouseOut(
    inspector.markup.imagePreviewTooltip,
    target
  );

  info("Updating the image src.");
  await updateImageSrc(img, UPDATED_SRC, inspector);

  target = container.editor.getAttributeElement("src").querySelector(".link");
  ok(target, "Found the src attribute in the markup view after mutation.");

  info("Showing tooltip on the src link.");
  await assertTooltipShownOnHover(inspector.markup.imagePreviewTooltip, target);

  info("Checking that the new image was shown.");
  checkImageTooltip(UPDATED_SRC_SIZE, inspector);

  await assertTooltipHiddenOnMouseOut(
    inspector.markup.imagePreviewTooltip,
    target
  );
});

/**
 * Updates the src attribute of the image. Return a Promise.
 */
function updateImageSrc(img, newSrc, inspector) {
  const onMutated = inspector.once("markupmutation");
  const onModified = img.modifyAttributes([
    {
      attributeName: "src",
      newValue: newSrc,
    },
  ]);

  return Promise.all([onMutated, onModified]);
}

/**
 * Checks that the markup view tooltip contains an image element with the given
 * size.
 */
function checkImageTooltip(size, { markup }) {
  const panel = markup.imagePreviewTooltip.panel;
  const images = panel.getElementsByTagName("img");
  is(images.length, 1, "Tooltip contains an image");

  const label = panel.querySelector(".devtools-tooltip-caption");
  is(label.textContent, size, "Tooltip label displays the right image size");

  markup.imagePreviewTooltip.hide();
}
