import { firstValueFrom } from 'rxjs';
import { SensitiveData } from './sensitive-data.service';
import { LocalStorageService } from 'src/app/local-storage.service';

describe('SensitiveData (privacy-safe defaults)', () => {
  function serviceWithStored(raw: string | null): SensitiveData {
    const storage = {
      getItem: () => raw,
      setBool: jasmine.createSpy('setBool'),
    } as unknown as LocalStorageService;
    return new SensitiveData(storage);
  }

  it('hides sensitive data by default on a fresh profile (no stored preference)', async () => {
    expect(await firstValueFrom(serviceWithStored(null).hidden)).toBeTrue();
  });

  it('respects an explicit stored "visible" choice', async () => {
    expect(await firstValueFrom(serviceWithStored('false').hidden)).toBeFalse();
  });

  it('respects an explicit stored "hidden" choice', async () => {
    expect(await firstValueFrom(serviceWithStored('true').hidden)).toBeTrue();
  });

  it('toggle persists the new state', async () => {
    const setBool = jasmine.createSpy('setBool');
    const storage = { getItem: () => null, setBool } as unknown as LocalStorageService;
    const service = new SensitiveData(storage);

    service.toggle();

    expect(await firstValueFrom(service.hidden)).toBeFalse();
    expect(setBool).toHaveBeenCalledWith('SENSITIVE_DATA_HIDDEN', false);
  });
});
