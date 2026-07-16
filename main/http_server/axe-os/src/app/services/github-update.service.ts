import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Observable } from 'rxjs';
import { map } from 'rxjs/operators';
import { NEURALAXE } from '../neuralaxe';


interface GithubRelease {
  id: number;
  tag_name: string;
  name: string;
  prerelease: boolean;
}

@Injectable({
  providedIn: 'root'
})
export class GithubUpdateService {

  /**
   * NeuralAxe OS release checks query the NeuralAxe repository only.
   * There is no fallback to upstream ESP-Miner releases: an official
   * upstream image manually uploaded by the user would replace NeuralAxe
   * branding, so upstream releases must never be offered as NeuralAxe updates.
   *
   * This request is only made after an explicit user action (see
   * UpdateComponent.handleReleaseCheck) — never in the background.
   */
  public static readonly RELEASES_URL =
    `https://api.github.com/repos/${NEURALAXE.updateRepository}/releases`;

  constructor(
    private httpClient: HttpClient
  ) { }


  public getReleases(): Observable<GithubRelease[]> {
    return this.httpClient.get<GithubRelease[]>(
      GithubUpdateService.RELEASES_URL
    ).pipe(
      map((releases: GithubRelease[]) => releases.filter((release: GithubRelease) => !release.prerelease))
    );
  }

}
