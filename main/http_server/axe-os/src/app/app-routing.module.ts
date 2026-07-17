import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';

import { AboutComponent } from './components/about/about.component';
import { HomeComponent } from './components/home/home.component';
import { LogsComponent } from './components/logs/logs.component';
import { SystemComponent } from './components/system/system.component';
import { UpdateComponent } from './components/update/update.component';
import { SettingsComponent } from './components/settings/settings.component';
import { NetworkComponent } from './components/network/network.component';
import { SwarmComponent } from './components/swarm/swarm.component';
import { ScoreboardComponent } from './components/scoreboard/scoreboard.component';
import { DesignComponent } from './components/design/design.component';
import { PoolComponent } from './components/pool/pool.component';
import { AppLayoutComponent } from './layout/app.layout.component';
import { ApModeGuard } from './guards/ap-mode.guard';
import { CommandDeckComponent } from './components/command-deck/command-deck.component';

const TITLE_PREFIX = 'NeuralAxe OS';

const routes: Routes = [
  {
      path: 'ap',
      component: AppLayoutComponent,
      children: [
        {
          path: '',
          component: NetworkComponent,
          title: `${TITLE_PREFIX} Network`,
        }
      ]
  },
  {
    path: '',
    component: AppLayoutComponent,
    canActivate: [ApModeGuard],
    children: [
      {
        path: '',
        component: CommandDeckComponent,
        title: TITLE_PREFIX,
      },
      {
        path: 'classic',
        component: HomeComponent,
        title: `${TITLE_PREFIX} Legacy Dashboard`,
      },
      {
        path: 'logs',
        component: LogsComponent,
        title: `${TITLE_PREFIX} Logs`,
      },
      {
        path: 'system',
        component: SystemComponent,
        title: `${TITLE_PREFIX} Device Status`,
      },
      {
        path: 'update',
        component: UpdateComponent,
        title: `${TITLE_PREFIX} Updates`,
      },
      {
        path: 'network',
        component: NetworkComponent,
        title: `${TITLE_PREFIX} Network`,
      },
      {
        path: 'settings',
        component: SettingsComponent,
        title: `${TITLE_PREFIX} Tuning & Thermal`,
      },
      {
        path: 'swarm',
        component: SwarmComponent,
        title: `${TITLE_PREFIX} Fleet`,
      },
      {
        path: 'scoreboard',
        component: ScoreboardComponent,
        title: `${TITLE_PREFIX} Scoreboard`,
      },
      {
        path: 'design',
        component: DesignComponent,
        title: `${TITLE_PREFIX} Display & Appearance`,
      },
      {
        path: 'pool',
        component: PoolComponent,
        title: `${TITLE_PREFIX} Pool`,
      },
      {
        path: 'about',
        component: AboutComponent,
        title: `${TITLE_PREFIX} About`,
      }
    ]
  },

];

@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule]
})
export class AppRoutingModule { }
