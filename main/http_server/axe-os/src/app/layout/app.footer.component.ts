import { Component } from '@angular/core';
import { LayoutService } from "./service/app.layout.service";
import { NEURALAXE } from '../neuralaxe';

@Component({
    selector: 'app-footer',
    templateUrl: './app.footer.component.html'
})
export class AppFooterComponent {
    public readonly neuralaxe = NEURALAXE;

    constructor(public layoutService: LayoutService) { }
}
