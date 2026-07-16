import { Component, ElementRef } from '@angular/core';
import { Observable } from 'rxjs';
import { LayoutService } from "./service/app.layout.service";
import { LiveDataService } from '../services/live-data.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { NEURALAXE } from 'src/app/neuralaxe';

@Component({
    selector: 'app-sidebar',
    templateUrl: './app.sidebar.component.html'
})
export class AppSidebarComponent {
    public readonly neuralaxe = NEURALAXE;
    public info$: Observable<ISystemInfo>;

    constructor(
        public layoutService: LayoutService,
        public el: ElementRef,
        private liveDataService: LiveDataService,
    ) {
        this.info$ = this.liveDataService.info$;
    }
}
