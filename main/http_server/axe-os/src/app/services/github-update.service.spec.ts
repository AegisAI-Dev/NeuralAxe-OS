import { TestBed } from '@angular/core/testing';

import { GithubUpdateService } from './github-update.service';
import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController } from '@angular/common/http/testing';
import { NEURALAXE } from '../neuralaxe';

describe('GithubUpdateService', () => {
  let service: GithubUpdateService;
  let httpMock: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(), provideHttpClientTesting()]
    });
    service = TestBed.inject(GithubUpdateService);
    httpMock = TestBed.inject(HttpTestingController);
  });

  afterEach(() => {
    // Fails the test if any unexpected request was made (e.g. upstream fallback).
    httpMock.verify();
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });

  it('should target the NeuralAxe repository, never upstream ESP-Miner', (done) => {
    expect(NEURALAXE.updateRepository).toBe('AegisAI-Dev/NeuralAxe-OS');
    expect(GithubUpdateService.RELEASES_URL)
      .toBe('https://api.github.com/repos/AegisAI-Dev/NeuralAxe-OS/releases');
    expect(GithubUpdateService.RELEASES_URL).not.toContain('bitaxeorg');
    expect(GithubUpdateService.RELEASES_URL).not.toContain('esp-miner');

    service.getReleases().subscribe(() => done());

    const req = httpMock.expectOne(GithubUpdateService.RELEASES_URL);
    expect(req.request.method).toBe('GET');
    req.flush([]);
  });

  it('should not fall back to another repository when no release exists', (done) => {
    service.getReleases().subscribe(releases => {
      expect(releases).toEqual([]);
      done();
    });

    // Exactly one request, to the NeuralAxe URL; verify() in afterEach
    // guarantees no second (fallback) request was issued.
    httpMock.expectOne(GithubUpdateService.RELEASES_URL).flush([]);
  });

  it('should filter out prereleases', (done) => {
    service.getReleases().subscribe(releases => {
      expect(releases.length).toBe(1);
      expect((releases[0] as any).tag_name).toBe('v0.1.0');
      done();
    });

    httpMock.expectOne(GithubUpdateService.RELEASES_URL).flush([
      { id: 2, tag_name: 'v0.2.0-rc1', name: 'rc', prerelease: true },
      { id: 1, tag_name: 'v0.1.0', name: 'stable', prerelease: false },
    ]);
  });

  it('should propagate errors without retrying elsewhere', (done) => {
    service.getReleases().subscribe({
      next: () => fail('expected an error'),
      error: () => done()
    });

    httpMock.expectOne(GithubUpdateService.RELEASES_URL)
      .flush({ message: 'rate limited' }, { status: 403, statusText: 'Forbidden' });
  });
});
