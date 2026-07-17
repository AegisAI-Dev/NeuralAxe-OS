import { Component, OnInit } from '@angular/core';
import { Observable } from 'rxjs';
import { LiveDataService } from '../services/live-data.service';
import { LayoutService } from './service/app.layout.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

@Component({
  selector: 'app-menu',
  templateUrl: './app.menu.component.html'
})
export class AppMenuComponent implements OnInit {
  public info$: Observable<ISystemInfo>;

  model: any[] = [];

  constructor(
    public layoutService: LayoutService,
    private liveDataService: LiveDataService
  ) {
    this.info$ = this.liveDataService.info$;
  }

  ngOnInit() {
    // NeuralAxe information architecture: grouped product sections instead of
    // the upstream flat page list. Route PATHS are unchanged — only labels and
    // grouping — so deep links and bookmarks keep working. The Bitcoin
    // whitepaper stays reachable from the About page (no top-level entry).
    this.model = [
      {
        label: 'Overview',
        items: [
          { label: 'Command Deck', icon: 'pi pi-fw pi-th-large', routerLink: ['/'] },
        ],
      },
      {
        label: 'Mining',
        items: [
          { label: 'Scoreboard', icon: 'pi pi-fw pi-trophy', routerLink: ['scoreboard'] },
          { label: 'Fleet', icon: 'pi pi-fw pi-sitemap', routerLink: ['swarm'] },
        ],
      },
      {
        label: 'Configuration',
        items: [
          { label: 'Pools', icon: 'pi pi-fw pi-server', routerLink: ['pool'] },
          { label: 'Network', icon: 'pi pi-fw pi-wifi', routerLink: ['network'] },
          { label: 'Tuning & Thermal', icon: 'pi pi-fw pi-sliders-h', routerLink: ['settings'] },
          { label: 'Display & Appearance', icon: 'pi pi-fw pi-palette', routerLink: ['design'] },
        ],
      },
      {
        label: 'System',
        items: [
          { label: 'Device Status', icon: 'pi pi-fw pi-wave-pulse', routerLink: ['system'] },
          { label: 'Logs', icon: 'pi pi-fw pi-list', routerLink: ['logs'] },
          { label: 'Updates', icon: 'pi pi-fw pi-sync', routerLink: ['update'] },
        ],
      },
      {
        label: 'Product',
        items: [
          { label: 'About NeuralAxe', icon: 'pi pi-fw pi-info-circle', routerLink: ['about'] },
        ],
      },
      {
        label: 'Advanced',
        items: [
          {
            label: 'Legacy Dashboard', icon: 'pi pi-fw pi-home', routerLink: ['classic'],
            title: 'Original upstream-compatible monitoring interface',
          },
        ],
      },
    ];
  }
}
