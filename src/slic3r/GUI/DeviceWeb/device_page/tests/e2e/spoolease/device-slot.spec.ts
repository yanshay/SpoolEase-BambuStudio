import { expect, test } from '@playwright/test';
import { mockAmsControlWebState } from '../../../src/features/device-page/ams-control-web/mockData';

test('renders and refreshes slot inventory without covering filament information', async ({ page }) => {
  await page.goto('/#/device_page/ams_control_web');
  const card = page.getByTestId('ams-slot-0-0');
  const slotContainer = card.locator('..');
  await expect(card).toBeVisible();
  await expect(slotContainer.locator('.spoolease-device-slot')).toHaveCount(0);

  const state = structuredClone(mockAmsControlWebState);
  const slot = state.display.ams_ext_area.units[0].slots[0];
  slot.fila_type = 'PETG-CF';
  slot.k_text = 'K 0.025';
  const sendState = async () => page.evaluate((payload) => {
    document.dispatchEvent(new CustomEvent('cpp:device', {
      detail: { head: { type: 'report' }, body: { module: 'device_page_ams_control_web', submod: 'state', payload } },
    }));
  }, state);

  await sendState();
  const material = card.getByText('PETG', { exact: true });
  const materialSuffix = card.getByText('-CF', { exact: true });
  const kValue = card.getByText('K 0.025', { exact: true });
  const glyph = card.getByRole('button', { name: /edit filament|view filament/i });
  const upstreamElements = [material, materialSuffix, kValue, glyph];
  const originalBoxes = await Promise.all(upstreamElements.map((element) => element.boundingBox()));
  const originalFonts = await Promise.all(upstreamElements.map((element) =>
    element.evaluate((node) => getComputedStyle(node).fontSize)));

  slot.spoolease = { spool_id: 'spool-12345', short_id: '2345', weight: '500g' };
  await sendState();
  await expect(slotContainer.locator('.spoolease-device-slot-id')).toHaveText('2345');
  await expect(slotContainer.locator('.spoolease-device-slot-weight')).toHaveText('500g');
  await expect(card).toHaveAttribute('title', /Spool ID: spool-12345\nNet weight: 500g/);
  await expect(card).toContainText('PETG');
  await expect(card).toContainText('-CF');
  await expect(card).toContainText('K 0.025');

  expect(await Promise.all(upstreamElements.map((element) => element.boundingBox()))).toEqual(originalBoxes);
  expect(await Promise.all(upstreamElements.map((element) =>
    element.evaluate((node) => getComputedStyle(node).fontSize)))).toEqual(originalFonts);
  const cardBox = await card.boundingBox();
  const idBox = await slotContainer.locator('.spoolease-device-slot-id').boundingBox();
  const weightBox = await slotContainer.locator('.spoolease-device-slot-weight').boundingBox();
  expect(cardBox).not.toBeNull();
  expect(idBox).not.toBeNull();
  expect(weightBox).not.toBeNull();
  expect(idBox!.y).toBeLessThan(cardBox!.y);
  expect(idBox!.y + idBox!.height).toBeGreaterThan(cardBox!.y + 3);
  expect(weightBox!.y).toBeLessThan(cardBox!.y + cardBox!.height);
  expect(weightBox!.y + weightBox!.height).toBeGreaterThan(cardBox!.y + cardBox!.height);

  slot.spoolease = { spool_id: 'spool-67890', short_id: '7890', weight: '32g' };
  await sendState();
  await expect(slotContainer.locator('.spoolease-device-slot-id')).toHaveText('7890');
  await expect(slotContainer.locator('.spoolease-device-slot-weight')).toHaveText('32g');

  slot.spoolease = undefined;
  await sendState();
  await expect(slotContainer.locator('.spoolease-device-slot')).toHaveCount(0);
  await expect(card).not.toHaveAttribute('title', /SpoolEase/);
});
